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
	float* apod_window;
	float* background_spectrum;
	fftwf_plan* fft_plan;  // if NULL, no FFT
};


typedef spsc_bounded_queue_t<aline_processing_job_msg> JobQueue; 


inline void process_alines(
	fftwf_complex* dst,
	uint16_t* src,
	int aline_size,  // Size of each A-line
	int number_of_alines,  // The total number of A-lines
	int roi_offset,  // The offset from the start of the spatial A-line to begin the axial ROI
	int roi_size,  // The number of voxels in the axial ROI
	fftwf_plan* fft_plan,  // FFTW plan
	WavenumberInterpolationPlan* interp_plan,  // Precalculated interpolation operator
	float* background_spectrum,  // Fixed pattern background spectrum to be subtracted 
	float* apod_window,  // Spectral shaping window to be multiplied
	void* fft_buffer,  // Buffer used for in-place FFT prior to cropping to the destination buffer
	float *interp_buffer  // Buffer used for A-line interpolation
)
{
	// Subtract background spectrum
	for (int i = 0; i < number_of_alines; i++)
	{
		for (int j = 0; j < aline_size; j++)
		{
			interp_buffer[j] = src[i * aline_size + j];  // Convert raw spectral data to float
			interp_buffer[j] -= background_spectrum[j];  // Subtract background/DC spectrum (will be zero if disabled)
		}
		// Apply wavenumber-linearization interpolation
		if (interp_plan != NULL)
		{
			interpdk_execute(interp_plan, interp_buffer, (float*)fft_buffer + i * aline_size);
		}
		else
		{
			memcpy((float*)fft_buffer + i * aline_size, interp_buffer, aline_size * sizeof(float));
		}
		// Multiply by apodization window
		for (int j = 0; j < aline_size; j++)
		{
			((float*)fft_buffer)[i * aline_size + j] *= apod_window[j];  // Will be 1 if disabled
		}
	}

	// FFT
	if (fft_plan != NULL)
	{
		fftwf_execute_dft_r2c(*(fft_plan), (float*)fft_buffer, (fftwf_complex*)fft_buffer);
	}
	// Cropping ROI and copying to output address
	for (int i = 0; i < number_of_alines; i++)
	{
		memcpy(dst + i * roi_size, (fftwf_complex*)fft_buffer + i * (aline_size / 2 + 1) + roi_offset, roi_size * sizeof(fftwf_complex));
	}
	// Normalize FFT result
	for (int i = 0; i < number_of_alines * roi_size; i++)
	{
		dst[i][0] /= aline_size;
		dst[i][1] /= aline_size;
	}
}


void aline_processing_worker(
	std::atomic_bool* running,  // Flag set by pool object which terminates thread
	JobQueue* queue,  // Pool object enqueues jobs here
	int aline_size,  // Size of each A-line
	int number_of_alines,  // The total number of A-lines
	int roi_offset,  // The offset from the start of the spatial A-line to begin the axial ROI
	int roi_size,  // The number of voxels in the axial ROI
	fftwf_plan* fft_plan,  // If NULL, no FFT and no axial cropping is performed.
	void* fft_buffer,  // Buffer used for in-place FFT prior to cropping to the destination buffer
	float* interp_buffer // A-line sized buffer used for interpolation result
)
{
	int spatial_aline_size = aline_size / 2 + 1;

	printf("Worker %i launched. Params: A-line size %i, Number of A-lines %i, Z ROI [%i %i]\n", std::this_thread::get_id(), aline_size, number_of_alines, roi_offset, roi_size);

	while (running->load() == true)
	{
		aline_processing_job_msg msg;
		if (queue->dequeue(msg))
		{

			process_alines(
				msg.dst_frame,
				msg.src_frame, 
				aline_size,
				number_of_alines,
				roi_offset,
				roi_size,
				fft_plan,
				msg.interp_plan,
				msg.background_spectrum,
				msg.apod_window,
				fft_buffer,
				interp_buffer
			);
			// printf("Worker %i finished writing to %p, about to increment barrier which is currently %i\n", std::this_thread::get_id(), msg.dst_frame, msg.barrier->load());
			msg.barrier->fetch_add(1);
		}
		else
		{
			Sleep(IDLE_SLEEP_MS);
			// printf("Worker %i polled an empty queue.\n", std::this_thread::get_id());
		}
	}

	printf("Worker %i terminated.\n", std::this_thread::get_id());
}


class AlineProcessingPool
{
private:

	std::atomic_bool _running;  // Flag which keeps all workers polling for new jobs.
	std::atomic_int _barrier;  // Determines when all workers have finished their jobs.

	std::vector<std::thread> pool;  // Vector of worker queues.
	std::vector<std::unique_ptr<JobQueue>> queues;  // Vector of worker messaging queues.

	WavenumberInterpolationPlan interpdk_plan;  // Wavenumber-linearization interpolation plan.
	fftwf_plan fft_plan;  // 32-bit FFTW DFT plan. NULL if disabled.

	void* fft_buffer;  // Buffer for the real-to-complex FFT before cropping
	std::unique_ptr<float[]> interp_buffer;

public:

	int aline_size;
	int64_t number_of_alines;
	int roi_offset;
	int roi_size;

	int spatial_aline_size;  // A-line size after real-to-complex FFT
	int64_t total_alines;
	int number_of_workers;
	int64_t alines_per_worker;

	AlineProcessingPool()
	{
		_running.store(false);
		number_of_workers = 0;
		total_alines = 0;
		alines_per_worker = 0;
	}

	AlineProcessingPool(
		int aline_size,  // Size of each A-line
		int64_t number_of_alines,  // The total number of A-lines
		int roi_offset,  // The offset from the start of the spatial A-line to begin the axial ROI
		int roi_size,  // The number of voxels in the axial ROI
		bool fft_enabled  // Whether or not to perform an FFT. If false, axial ROI cropping does not take place.
	)
	{
		printf("AlineProcessingPool initialized with A-line size: %i, number of A-lines: %i\n", aline_size, number_of_alines);

		// Need these for second constructor phase
		this->aline_size = aline_size;
		this->number_of_alines = number_of_alines;
		this->roi_offset = roi_offset;
		this->roi_size = roi_size;

		this->spatial_aline_size = aline_size / 2 + 1;

		total_alines = number_of_alines;

		if (number_of_alines > 512)
		{
			if (number_of_alines < 4096)
			{
				number_of_workers = 4;
			}
			if (number_of_alines < 1024)
			{
				number_of_workers = 2;
			}
			else
			{
				number_of_workers = std::thread::hardware_concurrency();
			}
			// Ensure number of threads is compatible with the number of A-lines. Worst case only 1 thread will be used.
			while ((number_of_alines % number_of_workers != 0) && (number_of_workers > 1))
			{
				number_of_workers -= 1;
			}
		}
		else  // Do the work in the calling thread if the job is sufficiently small
		{
			number_of_workers = 1;
			alines_per_worker = total_alines;
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
		// Allocate one contiguous buffer for the whole frame's transform and hand out chunks of it to the workers
		int64_t fft_buffer_size = (aline_size * alines_per_worker + 8 * alines_per_worker) * number_of_workers;
		fft_buffer = fftwf_alloc_real(fft_buffer_size);
		// The trasform will be in place, so the buffer will contain first real data and then complex
		printf("Allocated FFTW transform buffer.\n");
		interp_buffer = std::make_unique<float[]>(aline_size * number_of_workers);  // Single A-line sized buffer

		fftwf_import_wisdom_from_filename(".fftwf_wisdom");
		fftwf_set_timelimit(10.0);
		fft_plan = fftwf_plan_many_dft_r2c(1, n, alines_per_worker, (float*)fft_buffer, inembed, istride, idist, (fftwf_complex*)fft_buffer, onembed, ostride, odist, FFTW_PATIENT);
		if (fft_plan == NULL)
		{
			printf("Failed to generate FFTWF plan!\n");
		}
		else
		{
			printf("Generated FFTWF plan.\n");
			fftwf_export_wisdom_to_filename(".fftwf_wisdom");
		}
	}

	~AlineProcessingPool()
	{
		if (is_running())
		{
			terminate();
		}
		fftwf_free(fft_buffer);
		fftwf_destroy_plan(fft_plan);
	}

	// Submit a job to the pool. As only one job can be parallelized at one time by this pool, returns -1 if a job is already underway.
	int submit(
		fftwf_complex* dst_frame, // Pointer to destination buffer
		uint16_t* src_frame, // Pointer to raw frame
		bool interpolation_enabled, // Whether or not to perform wavenumber-linearization interpolation.
		double interpdk, // Wavenumber-linearization interpolation parameter.
		float* apodization_window,  // Window function to multiply spectral A-line by prior to FFT.
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
					fflush(stdout);
				}
			}
			if (number_of_workers > 1)
			{
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
			}
			else
			{
				process_alines(dst_frame, src_frame, aline_size, alines_per_worker, roi_offset, roi_size, &fft_plan, interpdk_plan_p, background_spectrum, apodization_window, fft_buffer, interp_buffer.get());
				_barrier++;
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
		_running.store(true);
		if (number_of_workers > 1)
		{
			printf("Spawning %i threads on %i cores, each processing %i of %i A-lines\n", number_of_workers, std::thread::hardware_concurrency(), alines_per_worker, total_alines);
			for (int i = 0; i < number_of_workers; i++)
			{
				queues.emplace_back( new JobQueue(32) );
				pool.push_back(std::thread(aline_processing_worker, &_running, queues.back().get(), aline_size, alines_per_worker, roi_offset, roi_size, &fft_plan, (float*)fft_buffer + (aline_size * alines_per_worker + 8 * alines_per_worker) * i, interp_buffer.get() + aline_size * i));
			}
		}
		else
		{
			printf("AlineProcessingPool in synchronous mode: Spawning zero new workers.\n");
		}
		// else do work in calling thread
		_barrier.store(number_of_workers);  // Set barrier to "finished" state.
	}

	// Stop and clean up the threads
	void terminate()
	{
		_running = false;
		if (number_of_workers > 1)
		{
			for (std::thread & th : pool)
			{
				if (th.joinable())
				{
					th.join();
				}
			}
			pool.clear();
			queues.clear();
		}
	}

};


