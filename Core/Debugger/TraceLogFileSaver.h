#pragma once
#include "pch.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/ITraceLogger.h"

struct ZSTD_CCtx_s;

struct CapturedTraceRow
{
	static constexpr size_t MaxCpuStateSize = 256;

	ITraceLogger* Logger = nullptr;
	DisassemblyInfo Disassembly;
	TraceLogPpuState PpuState = {};
	EffectiveAddressInfo EffectiveAddress = {};
	uint32_t MemoryValue = 0;
	bool HasEffectiveAddress = false;
	bool HasMemoryValue = false;
	alignas(16) uint8_t CpuState[MaxCpuStateSize];
};

class TraceLogFileSaver
{
private:
	static constexpr size_t CompressedOutputBufferSize = 1024 * 1024;
	static constexpr int ZstdCompressionLevel = 1;

	atomic<bool> _enabled = false;
	atomic<bool> _writeFailed = false;
	atomic<uint64_t> _droppedRows = 0;
	bool _compressed = false;
	ofstream _outputFile;
	vector<CapturedTraceRow> _captureBuffer;
	deque<vector<CapturedTraceRow>> _writeQueue;
	std::mutex _queueMutex;
	std::condition_variable _queueCondition;
	std::condition_variable _queueSpaceCondition;
	thread _writerThread;
	bool _stopWriter = false;
	bool _writerActive = false;
	static constexpr size_t MaxQueuedChunks = 8;
	static constexpr size_t TraceRecordBatchSize = 1024;

	ZSTD_CCtx_s* _zstdContext = nullptr;
	vector<char> _zstdOutputBuffer;
	vector<char> _compressedOutputBuffer;
	size_t _compressedOutputBufferPos = 0;

	bool HasZstdExtension(const string& filename) const;
	bool WriteToFile(const char* data, size_t size);
	bool FlushCompressedOutputBuffer();
	bool BufferCompressedOutput(const char* data, size_t size);
	bool WriteCompressed(const char* data, size_t size);
	bool FinalizeCompressedStream();
	bool WriteChunk(const string& chunk);
	void WriterLoop();
	void QueueCaptureBuffer(std::unique_lock<std::mutex>& lock);
	void CleanupCompression();

public:
	~TraceLogFileSaver();

	void StartLogging(string filename);
	void PrepareToStop();
	void StopLogging();
	void FlushPending();

	__forceinline bool IsEnabled() { return _enabled.load(std::memory_order_relaxed); }

	void Log(CapturedTraceRow&& row);
};
