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

	long long total_bytes_written;

	void open(const char* name) override
	{
		fout.open(name, std::ios::out | std::ios::binary);
		if (!fout) {
			std::cout << "Failed to open file: " << strerror(errno) << '\n';
		}
		total_bytes_written = 0;
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
		long written = 0;
		while (to_be_written > 0)
		{
			long n;
			if (to_be_written >= (long)WRITE_CHUNK_SIZE)
			{
				n = (long)WRITE_CHUNK_SIZE;
			}
			else
			{
				n = to_be_written;
			}
			fout.write((char*)f + written, n);
			written += n;
			to_be_written -= n;
		}
		if (!fout) {
			std::cout << "Failed to write to file: " << strerror(errno) << '\n';
		}

		QueryPerformanceCounter(&end);
		interval = (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;

		printf("fastnisdoct/FileStreamWorker: Wrote %li bytes to disk elapsed %f, %f GB/s, \n", frame_size, interval, ((double)frame_size / (double)BYTES_PER_GB) / (double)interval);
		total_bytes_written += written;  // Increment file object's counter
	}

	void close() override
	{
		fout.flush();
		fout.close();
	}

};


// Asynchronously stream frames from a CircAcqBuffer to disk
template <class T>
class FileStreamWorker
{
	private:
	
		std::thread _thread;
		std::atomic_bool _running = ATOMIC_VAR_INIT(false);
		std::atomic_bool _finished = ATOMIC_VAR_INIT(true);
		CircAcqBuffer<T>* _buffer;
		int _init_buffer_index;

		char _file_name[512];
		int _max_frames_per_file;

		FileStreamType _file_type;

		int _aline_size;
		long _alines_per_frame;
		float _file_max_gb;
		int _n_to_stream;
		long _frame_size_bytes;

		void _fstream()
		{
			int max_frames_per_file = (long long)((float)_file_max_gb * (float)BYTES_PER_GB) / _frame_size_bytes;
			if (max_frames_per_file < 1)
			{
				max_frames_per_file = 1;
			}
			int n_got;  // Index of locked out buffer
			T* frame;  // Pointer to locked out buffer

			// TODO various types
			std::unique_ptr<Writer> writer = std::make_unique<RawWriter>();
			const char* suffix = ".bin";

			int frames_in_current_file = 0;
			int file_name_inc = 0;
			int n_streamed = 0;

			int latest_frame_n;
			// Get ahead of buffer to let galvos settle
			if (_init_buffer_index == -1)
			{
				latest_frame_n = _buffer->get_count() + 5;
			}
			else
			{
				latest_frame_n = _init_buffer_index;
			}

			// Stream continuously to various files or until _n_to_stream is reached
			while (_running.load() && ((_n_to_stream > n_streamed) || (_n_to_stream == -1)))
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
						if ((_n_to_stream == -1) || (frames_in_current_file < _n_to_stream))  // Indefinite stream
						{
							// Append to file
							writer->writeFrame(frame, _frame_size_bytes);
							frames_in_current_file += 1;
							n_streamed += 1;
						}
						else
						{
							// Close file, stop streaming
							printf("fastnisdoct/FileStreamWorker: Closing file %s_%i%s after saving %i frames\n", _file_name, file_name_inc, suffix, frames_in_current_file);
							writer->close();
						}
						if (frames_in_current_file == max_frames_per_file)  // If this file cannot get larger, need to start a new one
						{
							file_name_inc += 1;
							if (writer->is_open())
							{
								printf("fastnisdoct/FileStreamWorker: Closing file %s_%i%s after saving %i frames\n", _file_name, file_name_inc, suffix, frames_in_current_file);
								writer->close();
							}
						}

					}
				}
				else  // Dropped frame, since we have fallen behind, get the latest next time
				{
					printf("fastnisdoct/FileStreamWorker: Writer can't keep up with acquisition rate! Dropped frame %i, got %i instead\n", latest_frame_n, n_got);
					latest_frame_n = _buffer->get_count() + 1;
				}
				_buffer->release();
			}
			if (writer->is_open())  // The stream has been stopped
			{
				// Close file
				printf("fastnisdoct/FileStreamWorker: Stream ended. Closing file %s after saving %i frames\n", _file_name, frames_in_current_file);
				writer->close();
			}
			_finished = true;
		}

		inline int _start
		(
			const char* fname,
			float max_gb,
			FileStreamType ftype,
			CircAcqBuffer<T>* buffer,
			int buffer_head,
			long frame_size,
			int n_to_stream
		)
		{
			if (is_streaming())
			{
				return -1;
			}
			if (_thread.joinable())
			{
				_thread.join();
			}
			_finished = false;
			_running = true;
			memcpy(_file_name, fname, strlen(fname) * sizeof(char) + 1);  // Copy string to module-managed buffer
			_file_max_gb = max_gb;
			_file_type = ftype;
			_buffer = buffer;
			_init_buffer_index = buffer_head;
			_frame_size_bytes = frame_size * sizeof(T);
			_n_to_stream = n_to_stream;
			printf("fastnisdoct: Starting FileStreamWorker: writing %i frames to %s, < %f GB/file\n", _n_to_stream, _file_name, _file_max_gb);
			_thread = std::thread(&FileStreamWorker::_fstream, this);  // Start the thread
		}

	public:

		bool is_streaming()
		{
			return _running.load() && !_finished.load();
		}

		int start
		(
			const char* fname,
			float max_gb,
			FileStreamType ftype,
			CircAcqBuffer<T>* buffer,
			long frame_size
		)
		{
			return _start(fname, max_gb, ftype, buffer, -1, frame_size, -1);
		}

		int start
		(
			const char* fname,
			float max_gb,
			FileStreamType ftype,
			CircAcqBuffer<T>* buffer,
			long frame_size,
			int n_to_stream
		)
		{
			return _start(fname, max_gb, ftype, buffer, -1, frame_size, n_to_stream);
		}

		int start
		(
			const char* fname,
			float max_gb,
			FileStreamType ftype,
			CircAcqBuffer<T>* buffer,
			int buf_head,
			long frame_size,
			int n_to_stream
		)
		{
			return _start(fname, max_gb, ftype, buffer, buf_head, frame_size, n_to_stream);
		}

		void stop()
		{
			if (_running)
			{
				_running.store(false);
				_thread.join();
			}
		}
};

