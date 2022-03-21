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

enum FileStreamMessageFlag
{
	StartStream = 1 << 1,
	StopStream = 1 << 2,
	StreamN = 1 << 3,
};

DEFINE_ENUM_FLAG_OPERATORS(FileStreamMessageFlag);

enum FileStreamType
{
	FSTREAM_TYPE_TIF = 1 << 1,
	FSTREAM_TYPE_NPY = 1 << 2,
	FSTREAM_TYPE_MAT = 1 << 3,
	FSTREAM_TYPE_RAW = 1 << 4
};

DEFINE_ENUM_FLAG_OPERATORS(FileStreamType);

struct FileStreamMessage
{
	FileStreamMessageFlag flag;
	const char* fname;
	int fsize;
	FileStreamType ftype;
	CircAcqBuffer<fftwf_complex>* circacqbuffer;
	int aline_size;  // Number of voxels in each A-line (ROI size)
	int number_of_alines;
	int n_to_stream;  // Number of frames to save for numbered stream
};

typedef spsc_bounded_queue_t<FileStreamMessage> FstreamQueue;

// Writers for various file types
// TODO write NPY, TIFF, MAT

class Writer
{
public:
	virtual void open(const char* name) {}
	virtual void writeFrame(void* f, int frame_size) {}
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

	void writeFrame(void* f, int frame_size) override
	{
		fout.write((char*)f, frame_size);
	}

	void close() override
	{
		fout.close();
	}

};


class FileStreamWorker final
{

protected:

	int id;

	std::thread fstream_thread;

	FstreamQueue* msg_queue;

	std::atomic_bool main_running;
	std::atomic_bool streaming;

	const char* file_name;

	int file_name_inc;
	int frames_in_current_file;
	int frames_per_file;

	FileStreamType file_type;
	CircAcqBuffer<fftwf_complex>* acq_buffer;
	int latest_frame_n;

	int aline_size;
	int number_of_alines;
	int file_max_bytes;
	int n_to_stream;

	inline void recv_msg()
	{
		FileStreamMessage msg;
		if (msg_queue->dequeue(msg))
		{
			if (msg.flag & StartStream)
			{
				if (!streaming.load())  // Ignore if already streaming
				{
					streaming.store(true);

					acq_buffer = msg.circacqbuffer;

					n_to_stream = -1; // If -1, indefinite stream

					file_name = msg.fname;
					file_type = msg.ftype;

					aline_size = msg.aline_size;
					number_of_alines = msg.number_of_alines;
					file_max_bytes = msg.fsize;

					file_name_inc = 0;
					frames_per_file = file_max_bytes / (aline_size * number_of_alines * (int)sizeof(fftwf_complex));

					latest_frame_n = acq_buffer->get_count();

				}
			}
			if (msg.flag & StreamN)
			{
				// printf("Numbered save: streaming %i frames to file %s.\n", msg.n_to_stream, file_name);
				n_to_stream = msg.n_to_stream;
			}
			if (msg.flag & StopStream)
			{
				if (streaming.load())
				{
					streaming.store(false);
				}
			}
		}
		else
		{
			return;
		}
	}

	void main()
	{

		bool fopen = false;
		int n_got;
		fftwf_complex* f;

		// TODO various types
		RawWriter file;
		const char* suffix = ".RAW";

		while (main_running.load() == true)
		{
			this->recv_msg();
			if (streaming.load())
			{
				n_got = acq_buffer->lock_out_wait(latest_frame_n, &f);

				if (latest_frame_n == n_got)
				{
					latest_frame_n += 1;

					if (!fopen)
					{
						// Open file
						char fname[MAX_PATH];
						if (file_name_inc == 0)
						{
							sprintf(fname, "%s%s", file_name, suffix);
						}
						else
						{
							sprintf(fname, "%s_%i%s", file_name, file_name_inc, suffix);
						}
						file.open(fname);
						frames_in_current_file = 0;
						fopen = true;
					}
					else
					{
						if (n_to_stream == -1)  // Indefinite stream
						{
							// Append to file
							file.writeFrame((void*)f, number_of_alines * aline_size * sizeof(fftwf_complex));
							frames_in_current_file += 1;
						}
						else if (frames_in_current_file < n_to_stream)  // Streaming n
						{
							file.writeFrame((void*)f, number_of_alines * aline_size * sizeof(fftwf_complex));
							frames_in_current_file += 1;
						}
						else
						{
							// Close file, stop streaming
							printf("Closing file %s after saving %i frames\n", file_name, frames_in_current_file);
							file.close();
							fopen = false;
							streaming.store(false);
						}

						if (frames_in_current_file == frames_per_file)  // If this file cannot get larger, need to start a new one
						{
							file_name_inc += 1;
							if (fopen)
							{
								printf("Closing file %s after saving %i frames\n", file_name, frames_in_current_file);
								file.close();
								fopen = false;
							}
						}

					}
				}
				else  // Dropped frame, since we have fallen behind, get the latest next time
				{
					printf("Dropped frame %i, got %i instead\n", latest_frame_n, n_got);
					latest_frame_n = acq_buffer->get_count();
				}
				acq_buffer->release();
			}
			else
			{
				if (fopen)  // The stream has been stopped by stopStream
				{
					// Close file
					printf("Closing file %s after saving %i frames\n", file_name, frames_in_current_file);
					file.close();
					fopen = false;
				}
			}
		}
	}

public:

	FileStreamWorker()
	{
		msg_queue = new FstreamQueue(0);
		main_running = ATOMIC_VAR_INIT(false);
		streaming = ATOMIC_VAR_INIT(false);
	}

	FileStreamWorker(int thread_id)
	{
		id = thread_id;

		msg_queue = new FstreamQueue(65536);
		main_running = ATOMIC_VAR_INIT(true);
		streaming = ATOMIC_VAR_INIT(false);
		fstream_thread = std::thread(&FileStreamWorker::main, this);
	}

	// DO NOT access non-atomics from outside main()

	bool is_streaming()
	{
		return streaming.load();
	}

	void start_streaming(const char* fname, int max_bytes, FileStreamType ftype, CircAcqBuffer<fftwf_complex>* buffer, int aline_size, int number_of_alines)
	{
		FileStreamMessage msg;
		msg.flag = StartStream;
		msg.fname = fname;
		msg.fsize = max_bytes;
		msg.circacqbuffer = buffer;
		msg.aline_size = aline_size;
		msg.number_of_alines = number_of_alines;
		msg_queue->enqueue(msg);
	}

	void start_streaming(const char* fname, int max_bytes, FileStreamType ftype, CircAcqBuffer<fftwf_complex>* buffer, int aline_size, int number_of_alines, int n_to_stream)
	{
		FileStreamMessage msg;
		msg.flag = StartStream | StreamN;
		msg.fname = fname;
		msg.fsize = max_bytes;
		msg.circacqbuffer = buffer;
		msg.aline_size = aline_size;
		msg.number_of_alines = number_of_alines;
		msg.n_to_stream = n_to_stream;
		msg_queue->enqueue(msg);
	}

	void stop_streaming()
	{
		FileStreamMessage msg;
		msg.flag = StopStream;
		msg_queue->enqueue(msg);
	}

	void terminate()
	{
		main_running.store(false);
		fstream_thread.join();
	}

};