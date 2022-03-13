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


void aline_processing_worker(std::atomic_bool running, spsc_bounded_queue_t<aline_processing_job_msg> msg_queue)
{
	while (running.load() == true)
	{
		printf("Worker %i is working...\n", std::this_thread::get_id());
	}
}


class AlineProcessingPool
{
private:
	std::atomic_bool _running;
	std::vector<std::thread> pool;

public:
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

	}

	void submit()
	{

	}

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
