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

spsc_bounded_queue_t<state_msg> msg_queue(32);

bool image_configured = false;
bool processing_configured = false;
bool scan_defined = false;

std::atomic_int state = STATE_UNOPENED;


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
			image_configured = true;
			if (ready_to_scan() && state != STATE_SCANNING)
			{
				state = STATE_READY;
			}
		}
		else if (msg.flag & MSG_CONFIGURE_PROCESSING)
		{
			printf("MSG_CONFIGURE_PROCESSING received\n");
			processing_configured = true;
			if (ready_to_scan() && state != STATE_SCANNING)
			{
				state = STATE_READY;
			}
		}
		else if (msg.flag & MSG_SET_PATTERN)
		{
			printf("MSG_SET_PATTERN received\n");
			scan_defined = true;
			if (ready_to_scan() && state != STATE_SCANNING)
			{
				state = STATE_READY;
			}
		}
		else if (msg.flag & MSG_START_SCAN)
		{
			printf("MSG_START_SCAN received\n");
			if (state.load() == STATE_READY)
			{
				state.store(STATE_SCANNING);
			}
		}
		else if (msg.flag & MSG_STOP_SCAN)
		{
			printf("MSG_STOP_SCAN received\n");
			if (state.load() == STATE_SCANNING)
			{
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
	state = STATE_OPEN;
	while (main_running)
	{
		recv_msg();
		if (state == STATE_UNOPENED || state == STATE_OPEN || state == STATE_READY)
		{
			Sleep(IDLE_SLEEP_TIME);  // Block for awhile if not scanning
		}
		else if (state == STATE_ERROR)
		{
			printf("Error!\n");
			return;
		}
		else  // if SCANNING or ACQUIRING
		{

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
		printf("nisdoct_open\n");
		printf("camera name: %s\n", cam_name);
		printf("ao x galvo name: %s\n", ao_x_ch);
		printf("ao y galvo name: %s\n", ao_y_ch);
		printf("ao lt galvo name: %s\n", ao_lt_ch);
		printf("ao ft galvo name: %s\n", ao_ft_ch);
		printf("ao st galvo name: %s\n", ao_st_ch);

		main_running = true;
		main_t = std::thread(&_main);
	}

	__declspec(dllexport) void nisdoct_close()
	{
		main_running = false;
		main_t.join();
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
		printf("nisdoct_configure_image\n");
		printf("dac_output_rate: %i\n", dac_output_rate);
		printf("aline_size: %i\n", aline_size);
		printf("number_of_alines: %i\n", number_of_alines);
		printf("alines_per_b: %i\n", alines_per_b);
		printf("aline_repeat: %i\n", aline_repeat);
		printf("bline_repeat: %i\n", bline_repeat);
		printf("number_of_buffers: %i\n", number_of_buffers);
		printf("roi_offset: %i\n", roi_offset);
		printf("roi_size: %i\n", roi_size);

		state_msg msg;
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
		printf("nisdoct_configure_processing\n");
		printf("fft_enabled: %i\n", fft_enabled);
		printf("subtract_background: %i\n", subtract_background);
		printf("interp: %i\n", interp);
		printf("intpdk: %f\n", intpdk);
		printf("apod_window: <%p>\n", apod_window);
		printf("a_rpt_proc_flag: %i\n", a_rpt_proc_flag);
		printf("b_rpt_proc_flag: %i\n", b_rpt_proc_flag);
		printf("n_frame_avg: %i\n", n_frame_avg);

		state_msg msg;
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
		printf("nisdoct_set_pattern\n");

		state_msg msg;
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
		printf("nisdoct_start_acquisition\n");
		printf("file: %s\n", file);
		printf("max_bytes (in GB): %i\n", max_bytes / (long long)1073741824);
		printf("n_frames_to_acquire: %i\n", n_frames_to_acquire);

		state_msg msg;
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


