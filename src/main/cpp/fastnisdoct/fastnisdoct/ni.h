#pragma once

// TODO remove. For testing
#include <stdlib.h>

// Functional interface for OCT frame grabbing and scanning using National Instruments libraries

int err;  // Error state of hardware interface

bool opened;  // If configured and connected to hardware
bool scanning;  // If actively scanning and grabbing frames

// IMAQ

/*
SESSION_ID session_id;  // IMAQ session ID
BUFLIST_ID buflist_id;  // IMAQ buflist ID
INTERFACE_ID interface_id;  // IMAQ interface ID
TaskHandle scan_task;  // NI-DAQ task handle
*/

int halfwidth; // A-line size / 2 + 1
int acqWinWidth;  // A-line size
int acqWinHeight;  // Number of A-lines per buffer
int bytesPerPixel;
int bufferSize;  // Equivalent to acqWinWidth * acqWinHeight * bytesPerPixel
int numberOfBuffers;  // Number of IMAQ ring buffer elements
int bufferNumber;  // Cumulative acquired IMAQ ring buffer element

unsigned int examined_number;  // Cumulative number of the currently examined IMAQ ring buffer element

// Int8** imaq_buffers;  // Ring buffer elements managed by IMAQ

// NI-DAQ

int dacRate;  // The output rate of the DAC used to drive the scan pattern
double* concatenated_scansig;  // Pointer to buffer of scansignals appended end to end
int scansig_n; // Number of samples in each of the 4 scan signals
int* samples_written;  // Returned by NI DAQ after samples are written


uint16_t* test_buffer = NULL;


namespace ni
{
	int setup_buffers(int aline_size, int number_of_alines, int number_of_buffers)
	{
		acqWinWidth = aline_size;
		acqWinHeight = number_of_alines;
		// Test code
		test_buffer = new uint16_t[acqWinWidth * acqWinHeight];
		printf("NI IMAQ buffers set up.\n");
		return 0;
	}

	int imaq_open(const char* camera_name)
	{
		printf("NI IMAQ interface opened.\n");
		return 0;
	}

	int imaq_close()
	{
		delete[] test_buffer;  // TODO remove
		printf("NI IMAQ interface closed.\n");
		return 0;
	}

	int daq_open(const char* aoScanX,
		const char* aoScanY,
		const char* aoLineTrigger,
		const char* aoFrameTrigger,
		const char* aoStartTrigger,
		int dac_rate
	)
	{
		printf("NI DAQ interface opened.\n");
		return 0;
	}

	int daq_close()
	{
		printf("NI DAQ interface closed.\n");
		return 0;
	}

	// These interact with both NI-IMAQ and NI-DAQmx APIs

	int start_scan()
	{
		printf("NI IMAQ scan started.\n");
		return 0;
	}

	int stop_scan()
	{
		printf("NI IMAQ scan stopped.\n");
		return 0;
	}

	int examine_buffer(uint16_t** raw_frame_addr, int frame_index)
	{
		printf("Attempting to lock out NI IMAQ buffer %i.\n", frame_index);
		// TEST
		for (int i = 0; i < acqWinWidth * acqWinHeight; i++)
		{
			test_buffer[i] = (uint16_t)std::rand() % 4099;
			// test_buffer[i] = 0;
		}
		raw_frame_addr = &test_buffer;
		printf("Test buffer generated!\n");
		return 0;
	}

	int release_buffer()
	{
		printf("NI IMAQ buffer released.\n");
		return 0;
	}

	int set_scan_pattern(double* x, double* y, double* linetrigger, double* frametrigger, int n)
	{
		printf("NI IMAQ scan signals buffered.\n");
		return 0;
	}
}
