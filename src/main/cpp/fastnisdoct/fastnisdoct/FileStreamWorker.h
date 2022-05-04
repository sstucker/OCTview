#pragma once

#define NOMINMAX

#include <thread>
#include <atomic>
#include "fftw3.h"
#include <condition_variable>
#include <complex>
#include <chrono>
#include "spscqueue.h"
#include "CircAcqBuffer.h"
#include <Windows.h>
#include <fstream>
#include <cerrno>

#define BYTES_PER_GB 1073741824
#define WRITE_CHUNK_SIZE 1048576
#define MAX_PATH 512


enum FileStreamType
{
	FSTREAM_TYPE_TIF = 1,
	FSTREAM_TYPE_NPY = 2,
	FSTREAM_TYPE_MAT = 3,
	FSTREAM_TYPE_RAW = 4
};

DEFINE_ENUM_FLAG_OPERATORS(FileStreamType);


class Writer
{
public:
	Writer() {}
	Writer(const Writer&) = default;
	virtual bool is_open() { return false; }
	virtual void open(const char* name) {}
	virtual void writeFrame(void* f, long frame_size) {}
	virtual void close() {}
};


class RawWriter : public Writer
{
private:
	std::ofstream fout;

public:

	void open(const char* name) override
	{
		fout.open(name, std::ios::out | std::ios::binary);
		if (!fout) {
			std::cout << "Failed to open file: " << strerror(errno) << '\n';
		}
	}

	bool is_open() override
	{
		return fout.is_open();
	}

	void writeFrame(void* f, long frame_size) override
	{
		LARGE_INTEGER frequency;
		LARGE_INTEGER start;
		LARGE_INTEGER end;
		double interval;

		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&start);  // Time the frame processing to make sure we should be able to keep up

		long to_be_written = frame_size;
		while (to_be_written > 0)
		{
			long n;
			if (to_be_written >= WRITE_CHUNK_SIZE)
				n = WRITE_CHUNK_SIZE;
			else
			{
				n = WRITE_CHUNK_SIZE - to_be_written;
			}
			fout.write((char*)f, n);
			to_be_written -= n;
		}
		if (!fout) {
			std::cout << "Failed to write to file: " << strerror(errno) << '\n';
		}

		QueryPerformanceCounter(&end);
		interval = (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;

		printf("Wrote %li bytes to disk elapsed %f, %f GB/s, \n", frame_size, interval, ((double)frame_size / (double)BYTES_PER_GB) / (double)interval);
	}

	void close() override
	{
		fout.close();
	}

};


std::thread _thread;
std::atomic_bool _running = ATOMIC_VAR_INIT(false);
std::atomic_bool _finished = ATOMIC_VAR_INIT(true);
CircAcqBuffer<fftwf_complex>* _buffer;

char _file_name[ 512 ];
int _max_frames_per_file;

FileStreamType _file_type;

int _aline_size;
long _alines_per_frame;
float _file_max_gb;
int _n_to_stream;


void _fstream()
{

	int max_frames_per_file = (long long)(_file_max_gb * BYTES_PER_GB) / (long long)(_aline_size * _alines_per_frame * (int)sizeof(fftwf_complex));
	if (max_frames_per_file < 1)
	{
		max_frames_per_file = 1;
	}
	int n_got;  // Index of locked out buffer
	fftwf_complex* frame;  // Pointer to locked out buffer

	// TODO various types
	Writer* writer = new RawWriter();
	const char* suffix = ".bin";

	int frames_in_current_file = 0;
	int file_name_inc = 0;
	int n_streamed = 0;

	// Get (a)head of buffer
	int latest_frame_n = _buffer->get_count() + 1;

	// Stream continuously to various files or until _n_to_stream is reached
	while (_running.load() && (_n_to_stream > n_streamed || _n_to_stream == -1))
	{
		n_got = _buffer->lock_out_wait(latest_frame_n, &frame);
		if (latest_frame_n == n_got)
		{
			latest_frame_n += 1;

			if (!writer->is_open())
			{
				// Open file
				char fname[MAX_PATH];
				if (file_name_inc == 0)
				{
					sprintf_s(fname, "%s%s", _file_name, suffix);
				}
				else
				{
					sprintf_s(fname, "%s_%04d%s", _file_name, file_name_inc, suffix);
				}
				writer->open(fname);
				frames_in_current_file = 0;
			}
			else  // If file is open
			{
				if (_n_to_stream == -1)  // Indefinite stream
				{
					// Append to file
					writer->writeFrame(frame, _alines_per_frame * (long)_aline_size * sizeof(fftwf_complex));
					frames_in_current_file += 1;
					n_streamed += 1;
				}
				else if (frames_in_current_file < _n_to_stream)  // Streaming n
				{
					writer->writeFrame(frame, _alines_per_frame * (long)_aline_size * sizeof(fftwf_complex));
					n_streamed += 1;
				}
				else
				{
					// Close file, stop streaming
					printf("Closing file %s_%i%s after saving %i frames\n", _file_name, file_name_inc, suffix, frames_in_current_file);
					writer->close();
				}
				if (frames_in_current_file == max_frames_per_file)  // If this file cannot get larger, need to start a new one
				{
					file_name_inc += 1;
					if (writer->is_open())
					{
						printf("Closing file %s_%i%s after saving %i frames\n", _file_name, file_name_inc, suffix, frames_in_current_file);
						writer->close();
					}
				}

			}
		}
		else  // Dropped frame, since we have fallen behind, get the latest next time
		{
			printf("Writer can't keep up! Not writing to file! Dropped frame %i, got %i instead\n", latest_frame_n, n_got);
			latest_frame_n = _buffer->get_count() + 1;
		}
		_buffer->release();
	}
	if (writer->is_open())  // The stream has been stopped
	{
		// Close file
		printf("Stream ended: Closing file %s after saving %i frames\n", _file_name, frames_in_current_file);
		writer->close();
	}
	delete writer;
	_finished = true;
}


inline int _start
(
	const char* fname,
	int max_gb,
	FileStreamType ftype,
	CircAcqBuffer<fftwf_complex>* buffer,
	int aline_size,
	int alines_per_frame,
	int n_to_stream
)
{
	if (_running.load() || !_finished.load())
	{
		return -1;
	}

	memcpy(_file_name, fname, strlen(fname) * sizeof(char) + 1);  // Copy string to module-managed buffer
	_file_max_gb = max_gb;
	_file_type = ftype;
	_buffer = buffer;
	_aline_size = aline_size;
	_alines_per_frame = alines_per_frame;
	_n_to_stream = n_to_stream;
	_running = true;
	printf("Starting FileStreamWorker: writing %i frames to %s, < %f GB/file\n", _n_to_stream, _file_name, _file_max_gb);
	_thread = std::thread(&_fstream);  // Start the thread
}

bool is_streaming()
{
	return _running.load();
}

int start_streaming
(
	const char* fname,
	long max_gb,
	FileStreamType ftype,
	CircAcqBuffer<fftwf_complex>* buffer,
	int aline_size,
	int alines_per_frame
)
{
	return _start(fname, max_gb, ftype, buffer, aline_size, alines_per_frame, -1);
}

int start_streaming
(
	const char* fname,
	float max_gb,
	FileStreamType ftype,
	CircAcqBuffer<fftwf_complex>* buffer,
	int aline_size,
	int alines_per_frame,
	int n_to_stream
)
{
	return _start(fname, max_gb, ftype, buffer, aline_size, alines_per_frame, n_to_stream);
}

void stop_streaming()
{
	_running.store(false);
	_thread.join();
}
