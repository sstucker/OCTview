#pragma once

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
	virtual void open(const char* name) {}
	virtual void writeFrame(void* f, int64_t frame_size) {}
	virtual void close() {}
};


class RawWriter : public Writer
{
private:
	std::ofstream fout;

public:

	void open(const char* name) override
	{
		fout = std::ofstream(name, std::ios::out | std::ios::binary);
	}

	void writeFrame(void* f, int64_t frame_size) override
	{
		fout.write((char*)f, frame_size);
	}

	void close() override
	{
		fout.close();
	}

};


std::thread _thread;
std::atomic_bool _running = ATOMIC_VAR_INIT(false);
CircAcqBuffer<fftwf_complex>* _buffer;

char _file_name[ 512 ];
int _frames_per_file;

FileStreamType _file_type;

int _aline_size;
int64_t _alines_per_frame;
int64_t _file_max_bytes;
int _n_to_stream;


void file_stream_worker()
{

	int frames_per_file = _file_max_bytes / (_aline_size * _alines_per_frame * (int)sizeof(fftwf_complex));
	int n_got;  // Index of locked out buffer
	fftwf_complex* f;  // Pointer to locked out buffer

	// TODO various types
	RawWriter file;
	const char* suffix = "";

	int frames_in_current_file = 0;
	int file_name_inc = 0;

	// Get head of buffer
	int latest_frame_n = _buffer->get_count();

	// Stream continuously to various files or until _n_to_stream is reached
	bool fopen = false;

	while (_running.load())
	{
		n_got = _buffer->lock_out_wait(latest_frame_n, &f);
		if (latest_frame_n == n_got)
		{
			latest_frame_n += 1;

			if (!fopen)
			{
				// Open file
				char fname[MAX_PATH];
				if (file_name_inc == 0)
				{
					sprintf_s(fname, "%s%s", _file_name, suffix);
				}
				else
				{
					sprintf_s(fname, "%s_%i%s", _file_name, file_name_inc, suffix);
				}
				file.open(fname);
				frames_in_current_file = 0;
				fopen = true;
			}
			else  // If file is open
			{
				if (_n_to_stream == -1)  // Indefinite stream
				{
					// Append to file
					file.writeFrame((void*)f, _alines_per_frame * _aline_size * sizeof(fftwf_complex));
					frames_in_current_file += 1;
				}
				else if (frames_in_current_file < _n_to_stream)  // Streaming n
				{
					file.writeFrame((void*)f, _alines_per_frame * _aline_size * sizeof(fftwf_complex));
					frames_in_current_file += 1;
				}
				else
				{
					// Close file, stop streaming
					printf("Closing file %s_%i%s after saving %i frames\n", _file_name, file_name_inc, suffix, frames_in_current_file);
					file.close();
					fopen = false;
				}
				if (frames_in_current_file == frames_per_file)  // If this file cannot get larger, need to start a new one
				{
					file_name_inc += 1;
					if (fopen)
					{
						printf("Closing file %s_%i%s after saving %i frames\n", _file_name, file_name_inc, suffix, frames_in_current_file);
						file.close();
						fopen = false;
					}
				}

			}
		}
		else  // Dropped frame, since we have fallen behind, get the latest next time
		{
			printf("Dropped frame %i, got %i instead\n", latest_frame_n, n_got);
			latest_frame_n = _buffer->get_count();
		}
		_buffer->release();
	}
	if (fopen)  // The stream has been stopped
	{
		// Close file
		printf("Closing file %s after saving %i frames\n", _file_name, frames_in_current_file);
		file.close();
		fopen = false;
	}
}


inline int _start
(
	const char* fname,
	int64_t max_bytes,
	FileStreamType ftype,
	CircAcqBuffer<fftwf_complex>* buffer,
	int aline_size,
	int alines_per_frame,
	int n_to_stream
)
{
	if (_running.load())
	{
		return -1;
	}

	memcpy(_file_name, fname, strlen(fname) * sizeof(char) + 1);  // Copy string to module-managed buffer
	_file_max_bytes = max_bytes;
	_file_type = ftype;
	_buffer = buffer;
	_aline_size = aline_size;
	_alines_per_frame = alines_per_frame;
	_n_to_stream = n_to_stream;
	_running = true;
	_thread = std::thread(file_stream_worker);  // Start the thread
}

int start_streaming
(
	const char* fname,
	int64_t max_bytes,
	FileStreamType ftype,
	CircAcqBuffer<fftwf_complex>* buffer,
	int aline_size,
	int alines_per_frame
)
{
	return _start(fname, max_bytes, ftype, buffer, aline_size, alines_per_frame, -1);
}

int start_streaming
(
	const char* fname,
	int64_t max_bytes,
	FileStreamType ftype,
	CircAcqBuffer<fftwf_complex>* buffer,
	int aline_size,
	int alines_per_frame,
	int n_to_stream
)
{
	return _start(fname, max_bytes, ftype, buffer, aline_size, alines_per_frame, n_to_stream);
}

void stop_streaming()
{
	_running.store(false);
}

void terminate()
{
	_running.store(false);
}
