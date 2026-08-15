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

bool TraceLogFileSaver::FlushOutputBuffer()
{
	if(_outputBuffer.empty()) {
		return true;
	}

	bool success;

	if(_compressed) {
		success = WriteCompressed(_outputBuffer.data(), _outputBuffer.size());
	} else {
		success = WriteToFile(_outputBuffer.data(), _outputBuffer.size());
	}

	_outputBuffer.clear();
	return success;
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

	_outputBuffer.clear();
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

	_enabled = true;
}

void TraceLogFileSaver::StopLogging()
{
	if(!_enabled && !_outputFile.is_open() && !_zstdContext) {
		return;
	}

	bool wasEnabled = _enabled;
	_enabled = false;

	if(_outputFile.is_open()) {
		bool success = true;

		if(wasEnabled) {
			success = FlushOutputBuffer();

			if(success && _compressed) {
				FinalizeCompressedStream();
			}
		}

		_outputFile.close();
	}

	CleanupCompression();
	_outputBuffer.clear();
}

void TraceLogFileSaver::Log(string& log)
{
	if(!_enabled) {
		return;
	}

	_outputBuffer += log + '\n';

	if(_outputBuffer.size() > TraceBufferFlushSize) {
		if(!FlushOutputBuffer()) {
			StopLogging();
		}
	}
}
