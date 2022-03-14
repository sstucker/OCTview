#pragma once

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

int ni_setup_buffers(int aline_size, int number_of_alines, int number_of_buffers)
{
	return 0;
}

int ni_imaq_open(const char* camera_name)
{
	return 0;
}

int ni_imaq_close()
{
	return 0;
}

int ni_daq_open(const char* aoScanX,
				const char* aoScanY,
				const char* aoLineTrigger,
				const char* aoFrameTrigger,
				const char* aoStartTrigger,
				int dac_rate
				)
{
	return 0;
}

int ni_daq_close()
{
	return 0;
}

// These interact with both NI-IMAQ and NI-DAQmx APIs

int ni_start_scan()
{
	return 0;
}

int ni_stop_scan()
{
	return 0;
}

int ni_examine_buffer(unsigned short** raw_frame_addr, int frame_index)
{
	return 0;
}

int ni_release_buffer()
{
	return 0;
}

int ni_set_scan_pattern(double* x, double* y, double* linetrigger, double* frametrigger, int n)
{
	return 0;
}
