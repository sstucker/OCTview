#pragma once

#include <thread>
#include <complex>
#include <atomic>
#include "spscqueue.h"
#include "fftw3.h"
#include "WavenumberInterpolationPlan.h"


struct aline_processing_job_msg {
	uint16_t* src_frame;
	std::complex<float>* dst_frame;
	std::atomic_int* barrier;
	int aline_size;
	int number_of_alines;
	float* mean_spectrum;
	bool fft;
	fftwf_plan* fft_plan;
	WavenumberInterpolationPlan* interp_plan;
};


typedef spsc_bounded_queue_t<aline_processing_job_msg> JobQueue;


void aline_processing_worker(std::atomic_bool* running, JobQueue* queue)
{
	while (running->load() == true)
	{
		// printf("Worker %i is working...\n", std::this_thread::get_id());
		aline_processing_job_msg msg;
		if (queue->dequeue(msg))
		{

		}
		else
		{
			// printf("Queue was empty!\n");
		}
		Sleep(1000);
		fflush(stdout);
	}
}


class AlineProcessingPool
{
private:

	std::atomic_bool _running;
	std::vector<std::thread> pool;
	std::vector<JobQueue*> queues;

public:

	int number_of_workers;
	int total_alines;
	int alines_per_worker;

	AlineProcessingPool()
	{
		_running.store(false);
		number_of_workers = 0;
		total_alines = 0;
		alines_per_worker = 0;
	}

	AlineProcessingPool(
		int aline_size,
		int number_of_alines,
		int roi_offset,
		int roi_size,
		bool subtract_background,
		bool interp,
		bool fft_enabled,
		double intpdk,
		float* apod_window
	)
	{
		printf("AlineProcessingPool init\n");

		total_alines = number_of_alines;

		number_of_workers = std::thread::hardware_concurrency();
		
		// Ensure number of threads is compatible with the number of A-lines. Worst case only 1 thread will be used.
		while ((number_of_alines % number_of_workers != 0) && (number_of_workers > 1))
		{
			number_of_workers -= 1;
		}

		alines_per_worker = total_alines / number_of_workers;
	}

	// Make changes to processing pipeline without reconfiguring
	void update(
		bool subtract_background,
		bool interp,
		bool fft_enabled,
		double intpdk,
		float* apod_window
	)
	{

	}

	// Submit a job to the pool
	void submit()
	{

	}

	// Start the threads
	void start()
	{
		printf("Spawning %i threads on %i cores, each processing %i of %i A-lines\n", number_of_workers, std::thread::hardware_concurrency(), alines_per_worker, total_alines);
		_running.store(true);
		for (int i = 0; i < number_of_workers; i++)
		{
			queues.push_back(new JobQueue(32));
			pool.push_back(std::thread(aline_processing_worker, &_running, queues.back()));
		}
	}

	// Stop and clean up the threads
	void terminate()
	{
		_running = false;
		for (std::thread & th : pool)
		{
			if (th.joinable())
			{
				th.join();
			}
		}
		pool.clear();
		for (JobQueue* p : queues)
			delete p;
		queues.clear();
	}

	bool running()
	{
		return _running.load();
	}

	~AlineProcessingPool()
	{
		terminate();
	}
};
