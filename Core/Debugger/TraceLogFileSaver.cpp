#include "pch.h"
#include "TraceLogFileSaver.h"

#include <cstring>
#include <zstd.h>

TraceLogFileSaver::~TraceLogFileSaver()
{
	StopLogging();
}

bool TraceLogFileSaver::HasZstdExtension(const string& filename) const
{
	constexpr const char* extension = ".zst";
	constexpr size_t extensionLength = 4;

	return filename.size() >= extensionLength &&
		filename.compare(filename.size() - extensionLength, extensionLength, extension) == 0;
}

bool TraceLogFileSaver::WriteToFile(const char* data, size_t size)
{
	if(size == 0) {
		return true;
	}

	_outputFile.write(data, size);
	return (bool)_outputFile;
}

bool TraceLogFileSaver::FlushCompressedOutputBuffer()
{
	if(_compressedOutputBufferPos == 0) {
		return true;
	}

	bool success = WriteToFile(_compressedOutputBuffer.data(), _compressedOutputBufferPos);
	_compressedOutputBufferPos = 0;
	return success;
}

bool TraceLogFileSaver::BufferCompressedOutput(const char* data, size_t size)
{
	while(size > 0) {
		size_t available = _compressedOutputBuffer.size() - _compressedOutputBufferPos;
		size_t copySize = size < available ? size : available;

		memcpy(
			_compressedOutputBuffer.data() + _compressedOutputBufferPos,
			data,
			copySize
		);

		_compressedOutputBufferPos += copySize;
		data += copySize;
		size -= copySize;

		if(_compressedOutputBufferPos == _compressedOutputBuffer.size()) {
			if(!FlushCompressedOutputBuffer()) {
				return false;
			}
		}
	}

	return true;
}

bool TraceLogFileSaver::WriteCompressed(const char* data, size_t size)
{
	ZSTD_inBuffer input = { data, size, 0 };

	while(input.pos < input.size) {
		ZSTD_outBuffer output = {
			_zstdOutputBuffer.data(),
			_zstdOutputBuffer.size(),
			0
		};

		size_t result = ZSTD_compressStream2(
			_zstdContext,
			&output,
			&input,
			ZSTD_e_continue
		);

		if(ZSTD_isError(result)) {
			return false;
		}

		if(output.pos > 0 &&
			!BufferCompressedOutput(_zstdOutputBuffer.data(), output.pos)) {
			return false;
		}
	}

	return true;
}

bool TraceLogFileSaver::FinalizeCompressedStream()
{
	ZSTD_inBuffer input = { nullptr, 0, 0 };
	size_t remaining;

	do {
		ZSTD_outBuffer output = {
			_zstdOutputBuffer.data(),
			_zstdOutputBuffer.size(),
			0
		};

		remaining = ZSTD_compressStream2(
			_zstdContext,
			&output,
			&input,
			ZSTD_e_end
		);

		if(ZSTD_isError(remaining)) {
			return false;
		}

		if(output.pos > 0 &&
			!BufferCompressedOutput(_zstdOutputBuffer.data(), output.pos)) {
			return false;
		}
	} while(remaining != 0);

	return FlushCompressedOutputBuffer();
}

bool TraceLogFileSaver::WriteChunk(const string& chunk)
{
	if(_compressed) {
		return WriteCompressed(chunk.data(), chunk.size());
	}
	return WriteToFile(chunk.data(), chunk.size());
}

void TraceLogFileSaver::WriterLoop()
{
	while(true) {
		vector<CapturedTraceRow> records;
		{
			std::unique_lock<std::mutex> lock(_queueMutex);
			_queueCondition.wait(lock, [&] { return _stopWriter || !_writeQueue.empty(); });
			if(_stopWriter) {
				break;
			}
			if(_writeQueue.empty()) {
				continue;
			}
			records = std::move(_writeQueue.front());
			_writeQueue.pop_front();
			_writerActive = true;
			_queueSpaceCondition.notify_one();
		}

		string chunk;
		chunk.reserve(records.size() * 200);
		for(CapturedTraceRow& record : records) {
			record.Logger->FormatCapturedRow(record, chunk);
		}

		if(!WriteChunk(chunk)) {
			{
				std::lock_guard<std::mutex> lock(_queueMutex);
				_writerActive = false;
				_writeFailed = true;
				_enabled = false;
			}
			_queueSpaceCondition.notify_all();
			break;
		}

		{
			std::lock_guard<std::mutex> lock(_queueMutex);
			_writerActive = false;
		}
		_queueSpaceCondition.notify_all();
	}
}

void TraceLogFileSaver::QueueCaptureBuffer(std::unique_lock<std::mutex>& lock)
{
	if(_captureBuffer.empty()) {
		return;
	}

	if(!_enabled) {
		return;
	}
	if(_writeQueue.size() >= MaxQueuedChunks) {
		_droppedRows += _captureBuffer.size();
		_captureBuffer.clear();
		return;
	}

	_writeQueue.emplace_back();
	_writeQueue.back().swap(_captureBuffer);
	_captureBuffer.reserve(TraceRecordBatchSize);
	_queueCondition.notify_one();
}

void TraceLogFileSaver::CleanupCompression()
{
	if(_zstdContext) {
		ZSTD_freeCCtx(_zstdContext);
		_zstdContext = nullptr;
	}

	_zstdOutputBuffer.clear();
	_compressedOutputBuffer.clear();
	_compressedOutputBufferPos = 0;
	_compressed = false;
}

void TraceLogFileSaver::StartLogging(string filename)
{
	StopLogging();

	_captureBuffer.clear();
	_captureBuffer.reserve(TraceRecordBatchSize);
	_compressed = HasZstdExtension(filename);

	_outputFile.open(filename, ios::out | ios::binary);
	if(!_outputFile) {
		_enabled = false;
		_compressed = false;
		return;
	}

	if(_compressed) {
		_zstdContext = ZSTD_createCCtx();

		if(!_zstdContext ||
			ZSTD_isError(ZSTD_CCtx_setParameter(
				_zstdContext,
				ZSTD_c_compressionLevel,
				ZstdCompressionLevel
			))) {
			CleanupCompression();
			_outputFile.close();
			_enabled = false;
			return;
		}

		_zstdOutputBuffer.resize(ZSTD_CStreamOutSize());
		_compressedOutputBuffer.resize(CompressedOutputBufferSize);
		_compressedOutputBufferPos = 0;
	}

	_stopWriter = false;
	_writerActive = false;
	_writeFailed = false;
	_droppedRows = 0;
	_enabled = true;
	_writerThread = thread(&TraceLogFileSaver::WriterLoop, this);
}

void TraceLogFileSaver::PrepareToStop()
{
	_enabled = false;
	_queueSpaceCondition.notify_all();
}

void TraceLogFileSaver::StopLogging()
{
	if(!_enabled && !_outputFile.is_open() && !_zstdContext) {
		return;
	}

	{
		std::lock_guard<std::mutex> lock(_queueMutex);
		_enabled = false;
		_captureBuffer.clear();
		_writeQueue.clear();
		_stopWriter = true;
	}
	_queueCondition.notify_one();
	_queueSpaceCondition.notify_all();
	if(_writerThread.joinable()) {
		_writerThread.join();
	}

	if(_outputFile.is_open()) {
		uint64_t droppedRows = _droppedRows.exchange(0);
		if(!_writeFailed && droppedRows > 0) {
			string droppedMessage = "[Mesen: " + std::to_string(droppedRows) + " trace rows dropped because the file writer could not keep up]\n";
			WriteChunk(droppedMessage);
		}
		if(!_writeFailed && _compressed) {
			FinalizeCompressedStream();
		}

		_outputFile.close();
	}

	CleanupCompression();
	_captureBuffer.clear();
	_writeQueue.clear();
}

void TraceLogFileSaver::FlushPending()
{
	if(!_enabled) {
		return;
	}

	std::unique_lock<std::mutex> lock(_queueMutex);
	QueueCaptureBuffer(lock);
	_queueSpaceCondition.wait(lock, [&] { return (_writeQueue.empty() && !_writerActive) || !_enabled; });
}

void TraceLogFileSaver::Log(CapturedTraceRow&& row)
{
	if(!_enabled) {
		return;
	}

	_captureBuffer.push_back(std::move(row));
	if(_captureBuffer.size() >= TraceRecordBatchSize) {
		std::unique_lock<std::mutex> lock(_queueMutex);
		QueueCaptureBuffer(lock);
	}
}
