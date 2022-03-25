#pragma once

#include <thread>
#include <complex>
#include <atomic>
#include "spscqueue.h"
#include "fftw3.h"
#include "WavenumberInterpolationPlan.h"

# define IDLE_SLEEP_MS 50

struct aline_processing_job_msg {
	std::complex<float>* dst_frame;
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
	fftwf_plan* fft_plan  // If NULL, no FFT and no axial cropping is performed.
)
{

	float* spectral_buffer = new float[aline_size * number_of_alines];
	float* intermediate_buffer = new float[aline_size * number_of_alines];
	fftwf_complex* spatial_buffer = fftwf_alloc_complex(aline_size * number_of_alines);

	int spatial_aline_size = aline_size / 2 + 1;  // Size of the A-line after R2C FFT

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
					spectral_buffer[i * aline_size + j] = (float)msg.src_frame[i * aline_size + j] - msg.background_spectrum[j];
				}
			}
			// Apply wavenumber-linearization interpolation
			if (msg.interp_plan != NULL)
			{
				for (int i = 0; i < number_of_alines; i++)
				{
					interpdk_execute(*msg.interp_plan, spectral_buffer + i * aline_size, intermediate_buffer + i * aline_size);
				}
			}
			// Multiply by apodization window
			for (int i = 0; i < number_of_alines; i++)
			{
				for (int j = 0; j < aline_size; j++)
				{
					intermediate_buffer[i * aline_size + j] *= msg.apod_window[j];
				}
			}
			// FFT
			if (msg.fft_plan != NULL)
			{
				fftwf_execute_dft_r2c(*(msg.fft_plan), intermediate_buffer, spatial_buffer);
			}
			// Cropping ROI and copying to output address
			for (int i = 0; i < number_of_alines; i++)
			{
				memcpy(msg.dst_frame + i * roi_size, (fftwf_complex*)spatial_buffer + i * spatial_aline_size + roi_offset, roi_size * sizeof(fftwf_complex));
			}

			printf("Worker %i finished, about to increment barrier which is currently %i\n", std::this_thread::get_id(), msg.barrier->load());
			*msg.barrier += 1;
		}
		else
		{
			Sleep(IDLE_SLEEP_MS);
			 // printf("Worker %i polled an empty queue.\n", std::this_thread::get_id());
		}
	}

	delete[] spectral_buffer;
	delete[] intermediate_buffer;
	fftwf_free(spatial_buffer);

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

	int roi_offset;
	int roi_size;

public:

	int aline_size;
	int spatial_aline_size;  // A-line size after real-to-complex FFT
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
		int odist = roi_size;
		int istride = 1;
		int ostride = 1;
		int* inembed = n;
		int* onembed = &odist;
		float* dummy_in = fftwf_alloc_real(aline_size * alines_per_worker + 8 * alines_per_worker);
		fftwf_complex* dummy_out = fftwf_alloc_complex(roi_size * total_alines);

		// fftwf_import_wisdom_from_filename("C:/Users/OCT/Dev/RealtimeOCT/octcontroller_fftw_wisdom.txt");

		fft_plan = fftwf_plan_many_dft_r2c(1, n, alines_per_worker, dummy_in, inembed, istride, idist, dummy_out, onembed, ostride, odist, FFTW_MEASURE);
		printf("Generated FFTWF plan.\n");

		fftwf_free(dummy_in);
		fftwf_free(dummy_out);
	}

	// Submit a job to the pool. As only one job can be parallelized at one time by this pool, returns -1 if a job is already underway.
	int submit(
		std::complex<float>* dst_frame, // Pointer to destination buffer
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
			if (interpolation_enabled)
			{
				if (interpdk == this->interpdk_plan.interpdk)  // If there has been no change to the interpolation parameter.
				{
					WavenumberInterpolationPlan* interpdk_plan_p = &this->interpdk_plan;
				}
				else
				{
					printf("Planning lambda->k interpolation... ");
					interpdk_plan = WavenumberInterpolationPlan(this->aline_size, interpdk);
					printf(" Finished.\n");
				}
			}
			else
			{
				WavenumberInterpolationPlan* interpdk_plan_p = NULL;
			}
			for (int i = 0; i < queues.size(); i++)
			{
				printf("Enqueuing job in JobQueue at %p\n", queues[i]);
				aline_processing_job_msg job;
				job.dst_frame = dst_frame + i * this->spatial_aline_size;
				job.src_frame = src_frame + i * this->aline_size;
				job.barrier = &_barrier;
				job.interp_plan = &interpdk_plan;
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
			pool.push_back(std::thread(aline_processing_worker, &_running, queues.back(), aline_size, alines_per_worker, roi_offset, roi_size, &fft_plan));
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
		}
		pool.clear();
		for (JobQueue* p : queues)
			delete p;
		queues.clear();
		fftwf_cleanup();
	}

	~AlineProcessingPool()
	{
		// terminate();
	}

};
