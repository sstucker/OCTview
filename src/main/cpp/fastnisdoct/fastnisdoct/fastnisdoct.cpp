// fastnisdoct.cpp : Defines the exported functions for the DLL application.
//

// #define WINVER 0x0502
// #define _WIN32_WINNT 0x0502

#include <Windows.h>
#include <stdio.h>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "spscqueue.h"
#include "AlineProcessingPool.h"
#include "CircAcqBuffer.h"
#include "ni.h"
#include <complex>

#define IDLE_SLEEP_TIME 1000

// msgs are passed into the main thread

#define MSG_CONFIGURE_IMAGE		  static_cast<int>( 1 << 0 )
#define MSG_CONFIGURE_PROCESSING  static_cast<int>( 1 << 1 )
#define MSG_SET_PATTERN           static_cast<int>( 1 << 2 )
#define MSG_START_SCAN			  static_cast<int>( 1 << 3 )
#define MSG_STOP_SCAN             static_cast<int>( 1 << 4 )
#define MSG_START_ACQUISITION     static_cast<int>( 1 << 5 )
#define MSG_STOP_ACQUISITION      static_cast<int>( 1 << 6 )

#define STATE_UNOPENED     		  static_cast<int>( 1 )
#define STATE_OPEN      		  static_cast<int>( 2 )
#define STATE_READY               static_cast<int>( 3 )
#define STATE_SCANNING            static_cast<int>( 4 )
#define STATE_ACQUIRIING		  static_cast<int>( 5 )
#define STATE_ERROR				  static_cast<int>( 6 )

struct raw_frame {
	const uint16_t* buffer;
	int aline_size;
	int number_of_alines;
};

struct processed_frame {
	const std::complex<float>* buffer;
	int aline_size;
	int number_of_alines;
};

struct state_msg {
	
	int flag;
	
	const char* cam_name;
	const char* ao_x_ch; 
	const char* ao_y_ch;
	const char* ao_lt_ch;
	const char* ao_ft_ch;
	const char* ao_st_ch;
	int dac_output_rate;
	int aline_size;
	int number_of_alines;
	int alines_per_b;
	int aline_repeat;
	int bline_repeat;
	int number_of_buffers;
	int roi_offset;
	int roi_size;
	bool fft_enabled;
	bool subtract_background;
	bool interp;
	double intpdk;
	float* apod_window;
	int a_rpt_proc_flag;
	int b_rpt_proc_flag;
	int n_frame_avg;
	double* x;
	double* y;
	int n_samples;
	double* line_trigger;
	double* frame_trigger;
	const char* file;
	int max_bytes;
	int n_frames_to_acquire;
};

inline void printf_state_data(state_msg s)
{
	printf("camera name: %s\n", s.cam_name);
	printf("ao x galvo name: %s\n", s.ao_x_ch);
	printf("ao y galvo name: %s\n", s.ao_y_ch);
	printf("ao lt galvo name: %s\n", s.ao_lt_ch);
	printf("ao ft galvo name: %s\n", s.ao_ft_ch);
	printf("ao st galvo name: %s\n", s.ao_st_ch);
	printf("dac_output_rate: %i\n", s.dac_output_rate);
	printf("aline_size: %i\n", s.aline_size);
	printf("number_of_alines: %i\n", s.number_of_alines);
	printf("alines_per_b: %i\n", s.alines_per_b);
	printf("aline_repeat: %i\n", s.aline_repeat);
	printf("bline_repeat: %i\n", s.bline_repeat);
	printf("number_of_buffers: %i\n", s.number_of_buffers);
	printf("roi_offset: %i\n", s.roi_offset);
	printf("roi_size: %i\n", s.roi_size);
	printf("fft_enabled: %i\n", s.fft_enabled);
	printf("subtract_background: %i\n", s.subtract_background);
	printf("interp: %i\n", s.interp);
	printf("intpdk: %f\n", s.intpdk);
	printf("apod_window: <%p>\n", s.apod_window);
	printf("a_rpt_proc_flag: %i\n", s.a_rpt_proc_flag);
	printf("b_rpt_proc_flag: %i\n", s.b_rpt_proc_flag);
	printf("n_frame_avg: %i\n", s.n_frame_avg);
	printf("file: %s\n", s.file);
	printf("max_bytes (in GB): %i\n", s.max_bytes / (long long)1073741824);
	printf("n_frames_to_acquire: %i\n", s.n_frames_to_acquire);
}

spsc_bounded_queue_t<state_msg> msg_queue(32);

AlineProcessingPool* aline_processing_pool;

bool image_configured = false;
bool processing_configured = false;
bool scan_defined = false;

std::atomic_int state = STATE_UNOPENED;
state_msg state_data;

int raw_frame_size = 0;  // Number of elements in the raw frame
int processed_frame_size = 0;  // Number of elements in the processed frame

CircAcqBuffer<uint16_t>* raw_frame_buffer = NULL;
CircAcqBuffer<std::complex<float>>* processed_frame_buffer = NULL;

inline bool ready_to_scan()
{
	return (image_configured && processing_configured && scan_defined);
}


inline void recv_msg()
{
	state_msg msg;
	if (msg_queue.dequeue(msg))
	{
		if (msg.flag & MSG_CONFIGURE_IMAGE)
		{
			printf("MSG_CONFIGURE_IMAGE received\n");
			if (state.load() == STATE_READY || state.load() == STATE_OPEN)
			{
				image_configured = false;
				state.store(STATE_OPEN);  // Always exit the READY state if configuring

				state_data.dac_output_rate = msg.dac_output_rate;
				state_data.aline_size = msg.aline_size;
				state_data.number_of_alines = msg.number_of_alines;
				state_data.alines_per_b = msg.alines_per_b;
				state_data.aline_repeat = msg.aline_repeat;
				state_data.bline_repeat = msg.bline_repeat;
				state_data.number_of_buffers = msg.number_of_buffers;
				state_data.roi_offset = msg.roi_offset;
				state_data.roi_size = msg.roi_size;

				// printf_state_data(state_data);
				processed_frame_size = state_data.roi_size * ((state_data.number_of_alines / state_data.aline_repeat) / state_data.bline_repeat);
				raw_frame_size = msg.aline_size * msg.number_of_alines;
				printf("Raw frame size: %i, processed frame size: %i\n", raw_frame_size, processed_frame_size);

				float total_buffer_size = msg.number_of_buffers * ((sizeof(uint16_t) * raw_frame_size) + (sizeof(std::complex<float>) * processed_frame_size));
				printf("Allocating %i ring buffers, total size %f GB...\n", msg.number_of_buffers, total_buffer_size / 1073741824.0);

				raw_frame_buffer = new CircAcqBuffer<uint16_t>(state_data.number_of_buffers, state_data.aline_size * state_data.number_of_alines);
				processed_frame_buffer = new CircAcqBuffer<std::complex<float>>(state_data.number_of_buffers, processed_frame_size);
				printf("Buffers allocated.\n");

				image_configured = true;

				if (ready_to_scan() && state == STATE_OPEN)
				{
					state.store(STATE_READY);
				}
			}
			else
			{
				printf("Cannot configure image! Not OPEN or READY.\n");
			}
		}
		else if (msg.flag & MSG_CONFIGURE_PROCESSING)
		{
			printf("MSG_CONFIGURE_PROCESSING received\n");
			if (image_configured)  // Image must be configured before processing can be configured
			{

				if (state.load() != STATE_SCANNING)  // If not scanning, update everything and reinitialize the A-line ThreadPool and processing buffers
				{
					processing_configured = false;
					state.store(STATE_OPEN);

					state_data.fft_enabled = msg.fft_enabled;
					state_data.subtract_background = msg.subtract_background;
					state_data.interp = msg.interp;
					state_data.intpdk = msg.intpdk;
					state_data.apod_window = msg.apod_window;
					state_data.a_rpt_proc_flag = msg.a_rpt_proc_flag;
					state_data.b_rpt_proc_flag = msg.b_rpt_proc_flag;
					state_data.n_frame_avg = msg.n_frame_avg;

					// Initialize the AlineProcessingPool. This creates an FFTW plan and may take a long time.
					aline_processing_pool = new AlineProcessingPool(
						state_data.aline_size,
						state_data.number_of_alines,
						state_data.roi_offset,
						state_data.roi_size,
						state_data.subtract_background,
						state_data.interp,
						state_data.fft_enabled,
						state_data.intpdk,
						state_data.apod_window
					);

					processing_configured = true;

					if (ready_to_scan() && state == STATE_OPEN)
					{
						state.store(STATE_READY);
					}
				}
				else if (state.load() == STATE_ACQUIRIING)
				{
					printf("Cannot configure processing duration acquisition.\n");
				}
				else  // If SCANNING, make real-time adjustments to apod window, background subtraction option, interpolation
				{
					state_data.subtract_background = msg.subtract_background;
					state_data.interp = msg.interp;
					state_data.intpdk = msg.intpdk;
					state_data.apod_window = msg.apod_window;
				}
			}
		}
		else if (msg.flag & MSG_SET_PATTERN)
		{
			printf("MSG_SET_PATTERN received\n");
			scan_defined = false;
			scan_defined = true;
			if (ready_to_scan() && state == STATE_OPEN)
			{
				state.store(STATE_READY);
			}
		}
		else if (msg.flag & MSG_START_SCAN)
		{
			printf("MSG_START_SCAN received\n");
			if (state.load() == STATE_READY)
			{
				state.store(STATE_SCANNING);
				aline_processing_pool->start();
			}
		}
		else if (msg.flag & MSG_STOP_SCAN)
		{
			printf("MSG_STOP_SCAN received\n");
			if (state.load() == STATE_SCANNING)
			{
				aline_processing_pool->terminate();
				state.store(STATE_READY);
			}
		}
		else if (msg.flag & MSG_START_ACQUISITION)
		{
			printf("MSG_START_ACQUISITION received\n");
			if (state.load() == STATE_SCANNING)
			{
				state.store(STATE_ACQUIRIING);
			}
		}
		else if (msg.flag & MSG_STOP_ACQUISITION)
		{
			printf("MSG_STOP_ACQUISITION received\n");
			if (state.load() == STATE_ACQUIRIING)
			{
				state.store(STATE_SCANNING);
			}
		}
		else
		{
			printf("Message flag %i not understood.\n", msg.flag);
		}
	}
	else
	{
		// printf("Queue was empty.\n");
	}
}


std::atomic_bool main_running = false;
std::thread main_t;
void _main()
{
	state.store(STATE_OPEN);
	while (main_running)
	{
		recv_msg();
		if (state.load() == STATE_UNOPENED || state.load() == STATE_OPEN || state.load() == STATE_READY)
		{
			Sleep(IDLE_SLEEP_TIME);  // Block for awhile if not scanning
		}
		else if (state.load() == STATE_ERROR)
		{
			printf("Error!\n");
			return;
		}
		else  // if SCANNING or ACQUIRING
		{
			// Lock out frame with IMAQ function
			// Send job to AlineProcessingPool
			// Calculate background spectrum (to be used with next scan)
			// Wait for job to finish
			// Perform A-line averaging or differencing
			// Perform B-line averaging or differencing
			// Perform frame averaging
			// if (state.load() == STATE_ACQUIRING)
			//		enqueue the frame with the StreamWorker
		}
		fflush(stdout);
	}
}


extern "C"  // DLL interface. Functions should enqueue messages or interact with the main_t.
{

	__declspec(dllexport) void nisdoct_open(
		const char* cam_name,
		const char* ao_x_ch,
		const char* ao_y_ch,
		const char* ao_lt_ch,
		const char* ao_ft_ch,
		const char* ao_st_ch
	)
	{
		main_running = true;
		main_t = std::thread(&_main);
	}

	__declspec(dllexport) void nisdoct_close()
	{
		main_running = false;
		main_t.join();
		delete aline_processing_pool;
	}

	__declspec(dllexport) void nisdoct_configure_image(
		int dac_output_rate,
		int aline_size,
		int number_of_alines,
		int alines_per_b,
		int aline_repeat,
		int bline_repeat,
		int number_of_buffers,
		int roi_offset,
		int roi_size
	)
	{
		state_msg msg;
		msg.dac_output_rate = dac_output_rate;
		msg.aline_size = aline_size;
		msg.number_of_alines = number_of_alines;
		msg.alines_per_b = alines_per_b;
		msg.aline_repeat = aline_repeat;
		msg.bline_repeat = bline_repeat;
		msg.number_of_buffers = number_of_buffers;
		msg.roi_offset = roi_offset;
		msg.roi_size = roi_size;
		msg.flag = MSG_CONFIGURE_IMAGE;
		msg_queue.enqueue(msg);
	}

	__declspec(dllexport) void nisdoct_configure_processing(
		bool fft_enabled,
		bool subtract_background,
		bool interp,
		double intpdk,
		float* apod_window,
		int a_rpt_proc_flag,
		int b_rpt_proc_flag,
		int n_frame_avg
	)
	{
		state_msg msg;
		msg.fft_enabled = fft_enabled;
		msg.subtract_background = subtract_background;
		msg.interp = interp;
		msg.intpdk = intpdk;
		msg.apod_window = apod_window;
		msg.a_rpt_proc_flag = a_rpt_proc_flag;
		msg.b_rpt_proc_flag = b_rpt_proc_flag;
		msg.n_frame_avg = n_frame_avg;
		msg.flag = MSG_CONFIGURE_PROCESSING;
		msg_queue.enqueue(msg);
	}

	__declspec(dllexport) void nisdoct_set_pattern(
		double* x,
		double* y,
		double* line_trigger,
		double* frame_trigger,
		int n_samples
	)
	{
		state_msg msg;
		msg.x = x;
		msg.y = y;
		msg.line_trigger = line_trigger;
		msg.frame_trigger = frame_trigger;
		msg.n_samples = n_samples;
		msg.flag = MSG_SET_PATTERN;
		msg_queue.enqueue(msg);
	}

	__declspec(dllexport) void nisdoct_start_scan()
	{
		printf("nisdoct_start_scan\n");

		state_msg msg;
		msg.flag = MSG_START_SCAN;
		msg_queue.enqueue(msg);
	}

	__declspec(dllexport) void nisdoct_stop_scan()
	{
		printf("nisdoct_stop_scan\n");

		state_msg msg;
		msg.flag = MSG_STOP_SCAN;
		msg_queue.enqueue(msg);
	}

	__declspec(dllexport) void nisdoct_start_acquisition(
		const char* file,
		long long max_bytes,
		int n_frames_to_acquire
	)
	{
		state_msg msg;
		msg.file = file;
		msg.max_bytes = max_bytes;
		msg.n_frames_to_acquire = n_frames_to_acquire;
		msg.flag = MSG_START_ACQUISITION;
		msg_queue.enqueue(msg);
	}

	__declspec(dllexport) void nisdoct_stop_acquisition()
	{
		printf("nisdoct_stop_acquisition\n");

		state_msg msg;
		msg.flag = MSG_STOP_ACQUISITION;
		msg_queue.enqueue(msg);
	}

	__declspec(dllexport) int nisdoct_get_state()
	{
		return state.load();
	}

	__declspec(dllexport) bool nisdoct_ready()
	{
		return (state.load() == STATE_READY);
	}

	__declspec(dllexport) bool nisdoct_scanning()
	{
		return (state.load() == STATE_SCANNING);
	}

	__declspec(dllexport) bool nisdoct_acquiring()
	{
		return (state.load() == STATE_ACQUIRIING);
	}

}


