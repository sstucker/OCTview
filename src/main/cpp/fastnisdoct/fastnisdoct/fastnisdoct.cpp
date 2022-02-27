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

#include "spscqueue.h"


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
	bool enabled;
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

std::atomic_int state = STATE_UNOPENED;

void recv_msg()
{
	state_msg msg;
	if (msg_queue.dequeue(msg))
	{
		if (msg.flag & MSG_CONFIGURE_IMAGE)
		{
			printf("MSG_CONFIGURE_IMAGE received\n");
		}
		else if (msg.flag & MSG_CONFIGURE_PROCESSING)
		{
			printf("MSG_CONFIGURE_PROCESSING received\n");
		}
		else if (msg.flag & MSG_SET_PATTERN)
		{
			printf("MSG_SET_PATTERN received\n");
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
		printf("Queue was empty.\n");
	}
}

std::atomic_bool main_running = false;
std::thread main_t;
void _main()
{
	while (main_running)
	{
		Sleep(1000);
		printf("Main is running. State is %i\n", state.load());
		recv_msg();
	}
}


extern "C"
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
		bool enabled,
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
		printf("enabled: %i\n", enabled);
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
		int max_bytes,
		int n_frames_to_acquire
	)
	{
		printf("nisdoct_start_acquisition\n");

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
		printf("get_state = %i\n", state.load());
		return state.load();
	}

	__declspec(dllexport) bool nisdoct_ready()
	{
		if (state.load() == STATE_READY)
		{
			return true;
		}
		else
		{
			return false;
		}
	}

	__declspec(dllexport) bool nisdoct_scanning()
	{
		if (state.load() == STATE_SCANNING)
		{
			return true;
		}
		else
		{
			return false;
		}
	}

	__declspec(dllexport) bool nisdoct_acquiring()
	{
		if (state.load() == STATE_ACQUIRIING)
		{
			return true;
		}
		else
		{
			return false;
		}
	}

}


