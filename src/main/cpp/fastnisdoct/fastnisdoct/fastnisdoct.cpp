// fastnisdoct.cpp : Defines the exported functions for the DLL application.
//

#define WINVER 0x0502
#define _WIN32_WINNT 0x0502

#include <Windows.h>
#include <stdio.h>
#include <iostream>

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
		printf("nisdoct_configure_image %i %i %i %i %i %i %i %i %i\n", dac_output_rate, aline_size, number_of_alines, alines_per_b, aline_repeat, bline_repeat, number_of_buffers, roi_offset, roi_size);
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
		printf("nisdoct_configure_processing %i %i %i %f %f %i %i %i\n", enabled, subtract_background, interp, intpdk, apod_window, a_rpt_proc_flag, b_rpt_proc_flag, n_frame_avg);
	}

}


