#pragma once
#include "pch.h"

struct ZSTD_CCtx_s;

class TraceLogFileSaver
{
private:
	static constexpr size_t TraceBufferFlushSize = 1024 * 1024;
	static constexpr size_t CompressedOutputBufferSize = 1024 * 1024;
	static constexpr int ZstdCompressionLevel = 1;

	bool _enabled = false;
	bool _compressed = false;
	bool _loggingError = false;
	string _outputBuffer;
	ofstream _outputFile;

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
	bool FlushOutputBuffer();
	void CleanupCompression();

public:
	~TraceLogFileSaver();

	void StartLogging(string filename);
	void StopLogging();

	__forceinline bool IsEnabled() { return _enabled; }

	void Log(const string& log);
};
