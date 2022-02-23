// fastnisdoct.cpp : Defines the exported functions for the DLL application.
//

#define WINVER 0x0502
#define _WIN32_WINNT 0x0502

#include <Windows.h>
#include <stdio.h>

extern "C"
{

	__declspec(dllexport) void nisdoct_open(const char* cam_name, const char* ao_x_ch, const char* ao_y_ch, const char* ao_lt_ch, const char* ao_ft_ch)
	{
		printf("nisdoct_open\n");
	}

}


