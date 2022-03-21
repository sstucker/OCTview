#pragma once

#include <thread>
#include <complex>
#include <atomic>
#include "spscqueue.h"
#include "fftw3.h"
#include "WavenumberInterpolationPlan.h"


struct aline_processing_job_msg {
	std::complex<float>* dst_frame;
	uint16_t* src_frame;
	std::atomic_int* barrier;
	WavenumberInterpolationPlan* interp_plan;  // if NULL, no interp
	const float* apod_window;
	fftwf_plan* fft_plan;  // if NULL, no FFT
};


typedef spsc_bounded_queue_t<aline_processing_job_msg> JobQueue;


void aline_processing_worker(
	std::atomic_bool* running,  // Flag set by pool object which terminates thread
	JobQueue* queue,  // Pool object enqueues jobs here
	int aline_size,  // Size of each A-line
	int number_of_alines,  // The total number of A-lines
	int roi_offset,  // The offset from the start of the spatial A-line to begin the axial ROI
	int roi_size,  // The number of voxels in the axial ROI
	fftwf_plan* fft_plan  // If NULL, no FFT and no axial cropping is performed.
)
{
	printf("Worker %i launched.\n", std::this_thread::get_id());
	printf("A-line size: %i, Number of A-lines: %i, Z ROI: [%i %i]\n", aline_size, number_of_alines, roi_offset, roi_size);
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

	std::atomic_bool _running;  // Flag which keeps all workers polling for new jobs.
	std::atomic_int _barrier;  // Determines when all workers have finished their jobs.

	std::vector<std::thread> pool;  // Vector of worker queues.
	std::vector<JobQueue*> queues;  // Vector of worker messaging queues.

	WavenumberInterpolationPlan interpdk_plan;  // Wavenumber-linearization interpolation plan.
	fftwf_plan fft_plan;  // 32-bit FFTW DFT plan. NULL if disabled.

	int roi_offset;
	int roi_size;

public:

	int aline_size;
	int total_alines;
	int number_of_workers;
	int alines_per_worker;
	double interpdk;

	AlineProcessingPool()
	{
		_running.store(false);
		number_of_workers = 0;
		total_alines = 0;
		alines_per_worker = 0;
	}

	AlineProcessingPool(
		int aline_size,  // Size of each A-line
		int number_of_alines,  // The total number of A-lines
		int roi_offset,  // The offset from the start of the spatial A-line to begin the axial ROI
		int roi_size,  // The number of voxels in the axial ROI
		bool fft_enabled  // Whether or not to perform an FFT. If false, axial ROI cropping does not take place.
	)
	{
		printf("AlineProcessingPool initialized with A-line size: %i, number of A-lines: %i\n", aline_size, number_of_alines);

		// Need these for second constructor phase
		this->aline_size = aline_size;
		this->roi_offset = roi_offset;
		this->roi_size = roi_size;

		total_alines = number_of_alines;

		number_of_workers = std::thread::hardware_concurrency();
		
		// Ensure number of threads is compatible with the number of A-lines. Worst case only 1 thread will be used.
		while ((number_of_alines % number_of_workers != 0) && (number_of_workers > 1))
		{
			number_of_workers -= 1;
		}

		alines_per_worker = total_alines / number_of_workers;
	}

	// Submit a job to the pool. As only one job can be parallelized at one time by this pool, returns -1 if a job is already underway.
	int submit(
		std::complex<float>* dst_frame, // Pointer to destination buffer
		uint16_t* src_frame, // Pointer to raw frame
		bool interpolation_enabled, // Whether or not to perform wavenumber-linearization interpolation.
		double interpdk, // Wavenumber-linearization interpolation parameter.
		const float* apodization_window  // Window function to multiply spectral A-line by prior to FFT.
	)
	{
		if (is_finished())
		{
			_barrier.store(0);
			if (interpolation_enabled)
			{
				if (interpdk == this->interpdk_plan.interpdk)  // If there has been no change to the interpolation parameter.
				{
					WavenumberInterpolationPlan* interpdk_plan_p = &this->interpdk_plan;
				}
				else
				{
					interpdk_plan = WavenumberInterpolationPlan(this->aline_size, interpdk);
				}
			}
			else
			{
				WavenumberInterpolationPlan* interpdk_plan_p = NULL;
			}
			for (JobQueue *q : queues)
			{
				aline_processing_job_msg job;
				job.dst_frame = dst_frame;
				job.src_frame = src_frame;
				job.barrier = &_barrier;
				job.interp_plan = &interpdk_plan;
				job.apod_window = apodization_window;
				job.fft_plan = &fft_plan;
				q->enqueue(job);
			}
		}
		else
		{
			return -1;
		}
	}

	bool is_running()
	{
		return _running.load();
	}

	// Poll the pool's barrier and see if the most submitted job is finished
	bool is_finished()
	{
		return (_barrier.load() >= number_of_workers);
	}

	// Start the threads
	void start()
	{
		printf("Spawning %i threads on %i cores, each processing %i of %i A-lines\n", number_of_workers, std::thread::hardware_concurrency(), alines_per_worker, total_alines);
		_running.store(true);
		for (int i = 0; i < number_of_workers; i++)
		{
			queues.push_back(new JobQueue(32));
			pool.push_back(std::thread(aline_processing_worker, &_running, queues.back(), aline_size, alines_per_worker, roi_offset, roi_size, &fft_plan));
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

	~AlineProcessingPool()
	{
		terminate();
	}

};
