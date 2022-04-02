#pragma once

#include <thread>
#include <complex>
#include <atomic>
#include "spscqueue.h"
#include "fftw3.h"
#include "WavenumberInterpolationPlan.h"

# define IDLE_SLEEP_MS 10

struct aline_processing_job_msg {
	fftwf_complex* dst_frame;
	uint16_t* src_frame;
	std::atomic_int* barrier;
	WavenumberInterpolationPlan* interp_plan;  // if NULL, no interp
	const float* apod_window;
	float* background_spectrum;
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
	fftwf_plan* fft_plan,  // If NULL, no FFT and no axial cropping is performed.
	void* fft_buffer  // Buffer used for in-place FFT prior to cropping to the destination buffer
)
{
	int spatial_aline_size = aline_size / 2 + 1;

	float* interp_buffer = new float[aline_size];  // Single A-line sized buffer

	printf("Worker %i launched. Params: A-line size %i, Number of A-lines %i, Z ROI [%i %i]\n", std::this_thread::get_id(), aline_size, number_of_alines, roi_offset, roi_size);

	while (running->load() == true)
	{
		aline_processing_job_msg msg;
		if (queue->dequeue(msg))
		{

			// Subtract background spectrum
			for (int i = 0; i < number_of_alines; i++)
			{
				for (int j = 0; j < aline_size; j++)
				{
					interp_buffer[j] = msg.src_frame[i * aline_size + j];  // Convert raw spectral data to float
					interp_buffer[j] -= -msg.background_spectrum[j];  // Subtract background/DC spectrum (will be zero if disabled)
				}
				// Apply wavenumber-linearization interpolation
				if (msg.interp_plan != NULL)
				{
					interpdk_execute(msg.interp_plan, interp_buffer, (float*)fft_buffer + i * aline_size);
				}
				else
				{
					memcpy(interp_buffer + i * aline_size, (float*)fft_buffer + i * aline_size, aline_size * number_of_alines * sizeof(float));
				}
				// Multiply by apodization window
				for (int j = 0; j < aline_size; j++)
				{
					((float*)fft_buffer)[i * aline_size + j] *= msg.apod_window[j];  // Will be 1 if disabled
				}
			}

			// FFT
			if (msg.fft_plan != NULL)
			{
				fftwf_execute_dft_r2c(*(msg.fft_plan), (float*)fft_buffer, (fftwf_complex*)fft_buffer);
			}
			// Cropping ROI and copying to output address
			for (int i = 0; i < number_of_alines; i++)
			{
				memcpy(msg.dst_frame + i * roi_size, (fftwf_complex*)fft_buffer + i * spatial_aline_size + roi_offset, roi_size * sizeof(fftwf_complex));
			}
			// Normalize FFT result
			for (int i = 0; i < number_of_alines * roi_size; i++)
			{
				msg.dst_frame[i][0] /= aline_size;
				msg.dst_frame[i][1] /= aline_size;
			}
			// printf("Worker %i finished writing to %p, about to increment barrier which is currently %i\n", std::this_thread::get_id(), msg.dst_frame, msg.barrier->load());
			++*msg.barrier;
		}
		else
		{
			Sleep(IDLE_SLEEP_MS);
			// printf("Worker %i polled an empty queue.\n", std::this_thread::get_id());
		}
	}

	delete[] interp_buffer;

	printf("Worker %i terminated.\n", std::this_thread::get_id());
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

	void* fft_buffer;  // Buffer for the real-to-complex FFT before cropping

	int roi_offset;
	int roi_size;

public:

	int aline_size;
	int spatial_aline_size;  // A-line size after real-to-complex FFT
	int total_alines;
	int number_of_workers;
	int alines_per_worker;

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
		this->spatial_aline_size = aline_size / 2 + 1;
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

		// FFTW "many" plan
		int n[] = { aline_size };
		int idist = aline_size;
		int odist = spatial_aline_size;
		int istride = 1;
		int ostride = 1;
		int* inembed = n;
		int* onembed = &odist;
		fft_buffer = fftwf_alloc_real((aline_size * alines_per_worker + 8 * alines_per_worker) * number_of_workers);
		printf("Allocated FFTW transform buffer.\n");

		// fftwf_import_wisdom_from_filename("C:/Users/OCT/Dev/RealtimeOCT/octcontroller_fftw_wisdom.txt");

		fft_plan = fftwf_plan_many_dft_r2c(1, n, alines_per_worker, (float*)fft_buffer, inembed, istride, idist, (fftwf_complex*)fft_buffer, onembed, ostride, odist, FFTW_MEASURE);
		if (fft_plan == NULL)
		{
			printf("Failed to generate FFTWF plan!\n");
		}
		printf("Generated FFTWF plan.\n");
	}

	~AlineProcessingPool()
	{
		fftwf_free(fft_buffer);
		fftwf_destroy_plan(fft_plan);
		fftwf_cleanup();
	}

	// Submit a job to the pool. As only one job can be parallelized at one time by this pool, returns -1 if a job is already underway.
	int submit(
		fftwf_complex* dst_frame, // Pointer to destination buffer
		uint16_t* src_frame, // Pointer to raw frame
		bool interpolation_enabled, // Whether or not to perform wavenumber-linearization interpolation.
		double interpdk, // Wavenumber-linearization interpolation parameter.
		const float* apodization_window,  // Window function to multiply spectral A-line by prior to FFT.
		float* background_spectrum  // Spectrum to subtract from each raw spectrum prior to multiplication by the apod window
	)
	{
		if (is_finished())
		{
			_barrier.store(0);
			WavenumberInterpolationPlan* interpdk_plan_p = NULL;
			if (interpolation_enabled)
			{
				if (interpdk == this->interpdk_plan.interpdk)  // If there has been no change to the interpolation parameter.
				{
					interpdk_plan_p = &this->interpdk_plan;
				}
				else
				{
					printf("Planning lambda->k interpolation... ");
					this->interpdk_plan = WavenumberInterpolationPlan(this->aline_size, interpdk);
					interpdk_plan_p = &this->interpdk_plan;
					printf(" Finished.\n");
				}
			}
			for (int i = 0; i < queues.size(); i++)
			{
				// printf("Enqueuing job in JobQueue at %p\n", queues[i]);
				aline_processing_job_msg job;
				job.dst_frame = dst_frame + i * this->roi_size * this->alines_per_worker;
				job.src_frame = src_frame + i * this->aline_size * this->alines_per_worker;
				job.barrier = &_barrier;
				job.interp_plan = interpdk_plan_p;
				job.apod_window = apodization_window;
				job.background_spectrum = background_spectrum;
				job.fft_plan = &fft_plan;
				queues[i]->enqueue(job);
			}
			return 0;
		}
		else
		{
			printf("Failed to submit job... pool not finished with previous!\n");
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

	int join()
	{
		while (!is_finished()) {}  // Spin on the barrier
		return 0;
		// TODO error state, timeout
	}

	// Start the threads
	void start()
	{
		printf("Spawning %i threads on %i cores, each processing %i of %i A-lines\n", number_of_workers, std::thread::hardware_concurrency(), alines_per_worker, total_alines);
		_running.store(true);
		for (int i = 0; i < number_of_workers; i++)
		{
			queues.push_back(new JobQueue(32));
			pool.push_back(std::thread(aline_processing_worker, &_running, queues.back(), aline_size, alines_per_worker, roi_offset, roi_size, &fft_plan, (float*)fft_buffer + (aline_size * alines_per_worker + 8 * alines_per_worker) * i));
		}
		_barrier.store(number_of_workers);  // Set barrier to "finished" state.
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
			else
			{
				printf("Worker thread wasn't joinable!!\n");
			}
		}
		pool.clear();
		for (int i = 0; i < number_of_workers; i++)
		{
			delete queues[i];
		}
		queues.clear();
	}

};


