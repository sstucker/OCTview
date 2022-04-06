#pragma once

#include "niimaq.h"
#include "NIDAQmx.h"

// TODO remove. For testing
#include <stdlib.h>
#include <time.h>

// Functional interface for OCT frame grabbing and scanning using National Instruments libraries

int err;  // Error state of hardware interface

// IMAQ

SESSION_ID session_id;  // IMAQ session ID
BUFLIST_ID buflist_id;  // IMAQ buflist ID
INTERFACE_ID interface_id;  // IMAQ interface ID
TaskHandle scan_task;  // NI-DAQ task handle

int acqWinWidth;  // A-line size
int acqWinHeight;  // Number of A-lines per buffer
int bytesPerPixel;
int bufferSize;  // Equivalent to acqWinWidth * acqWinHeight * bytesPerPixel
int numberOfBuffers = 0;  // Number of IMAQ ring buffer elements
int bufferNumber;  // Cumulative acquired IMAQ ring buffer element

uInt32 examined_number;  // Cumulative number of the currently examined IMAQ ring buffer element

Int8** imaq_buffers;  // Ring buffer elements managed by IMAQ

uint16_t** buffers = NULL;  // Ring buffer elements allocated manually

// NI-DAQ

int dac_rate = 0;  // The output rate of the DAC used to drive the scan pattern
float64* concatenated_scansig;  // Pointer to buffer of scansignals appended end to end
int32 scansig_n = 0; // Number of samples in each of the 4 scan signals
int32 samples_written = 0;  // Returned by NI DAQ after samples are written


class ScanPattern
{

public:

	double* x;
	double* y;
	double* line_trigger;
	double* frame_trigger;
	int n;
	int rate;

	ScanPattern(double* x, double* y, double* line_trigger, double* frame_trigger, int n, int rate)
	{
		this->x = new double[n];
		memcpy(this->x, x, sizeof(double) * n);

		this->y = new double[n];
		memcpy(this->y, y, sizeof(double) * n);

		this->line_trigger = new double[n];
		memcpy(this->line_trigger, line_trigger, sizeof(double) * n);

		this->frame_trigger = new double[n];
		memcpy(this->frame_trigger, frame_trigger, sizeof(double) * n);

		this->n = n;
		this->rate = rate;
	}

	~ScanPattern()
	{
		delete[] this->x;
		delete[] this->y;
		delete[] this->line_trigger;
		delete[] this->frame_trigger;
	}

};


namespace ni
{

	inline void print_error_msg()
	{
		if (err != 0)
		{
			char* buf = new char[512];
			DAQmxGetErrorString(err, buf, 512);
			printf(buf);
			printf("\n");
			delete[] buf;
		}
		else
		{
			printf("No error.\n");
		}
	}

	int imaq_buffer_cleanup()
	{
		for (int i = 0; i < numberOfBuffers; i++)
		{
			delete[] buffers[i];
		}
		delete buffers;
		buffers = NULL;
		return 0;
	}

	int setup_buffers(int aline_size, int number_of_alines, int number_of_buffers)
	{
		err = imgSetAttribute2(session_id, IMG_ATTR_ACQWINDOW_TOP, 0);
		err = imgSetAttribute2(session_id, IMG_ATTR_ACQWINDOW_LEFT, 0);
		err = imgSetAttribute2(session_id, IMG_ATTR_ACQWINDOW_HEIGHT, number_of_alines);
		err = imgSetAttribute2(session_id, IMG_ATTR_ACQWINDOW_WIDTH, aline_size);
		err = imgSetAttribute2(session_id, IMG_ATTR_BYTESPERPIXEL, 2);

		// Confirm the change by getting the attributes
		err = imgGetAttribute(session_id, IMG_ATTR_ROI_WIDTH, &acqWinWidth);
		err = imgGetAttribute(session_id, IMG_ATTR_ROI_HEIGHT, &acqWinHeight);
		err = imgGetAttribute(session_id, IMG_ATTR_BYTESPERPIXEL, &bytesPerPixel);

		if (err != 0)
		{
			return err;
		}

		bufferSize = acqWinWidth * acqWinHeight * bytesPerPixel;
		if (buffers != NULL)
		{
			imaq_buffer_cleanup();
		}
		buffers = new uint16_t*[number_of_buffers];
		for (int i = 0; i < number_of_buffers; i++)
		{
			buffers[i] = new uint16_t[aline_size * number_of_alines];
		}
		if (number_of_buffers > 0)
		{
			imgRingSetup(session_id, number_of_buffers, (void**)buffers, 0, 0);
		}
		numberOfBuffers = number_of_buffers;
		return err;
	}

	int imaq_open(const char* camera_name)
	{
		err = imgInterfaceOpen(camera_name, &interface_id);
		if (err != 0)
		{
			return err;
		}
		err = imgSessionOpen(interface_id, &session_id);
		if (err != 0)
		{
			return err;
		}
		// Configure the frame acquisition to be triggered by the TTL1 line
		err = imgSetAttribute2(session_id, IMG_ATTR_EXT_TRIG_LINE_FILTER, true);
		// Frame trigger TTL1
		err = imgSessionTriggerConfigure2(session_id, IMG_SIGNAL_EXTERNAL, IMG_EXT_TRIG1, IMG_TRIG_POLAR_ACTIVEH, 1000, IMG_TRIG_ACTION_BUFFER);
		// Frame trigger output TTL2
		err = imgSessionTriggerDrive2(session_id, IMG_SIGNAL_EXTERNAL, IMG_EXT_TRIG2, IMG_TRIG_POLAR_ACTIVEH, IMG_TRIG_DRIVE_FRAME_START);
		return err;
	}

	int imaq_close()
	{
		imaq_buffer_cleanup();
		err = imgClose(session_id, TRUE);
		err = imgClose(interface_id, TRUE);
		return err;
	}

	int daq_open(
		const char* aoScanX,
		const char* aoScanY,
		const char* aoLineTrigger,
		const char* aoFrameTrigger,
		const char* aoStartTrigger
	)
	{
		err = DAQmxCreateTask("scan", &scan_task);
		err = DAQmxCreateAOVoltageChan(scan_task, aoScanX, "", -10, 10, DAQmx_Val_Volts, NULL);
		err = DAQmxCreateAOVoltageChan(scan_task, aoScanY, "", -10, 10, DAQmx_Val_Volts, NULL);
		err = DAQmxCreateAOVoltageChan(scan_task, aoLineTrigger, "", -10, 10, DAQmx_Val_Volts, NULL);
		err = DAQmxCreateAOVoltageChan(scan_task, aoFrameTrigger, "", -10, 10, DAQmx_Val_Volts, NULL);

		if (err != 0)
		{
			printf("DAQmx failed to create task with channels %s, %s, %s, %s:\n", aoScanX, aoScanY, aoLineTrigger, aoFrameTrigger);
			char* buf = new char[512];
			DAQmxGetErrorString(err, buf, 512);
			printf(buf);
			printf("\n");
			delete[] buf;
			return err;
		}
		else
		{
			dac_rate = 76000 * 2;
			err = DAQmxSetWriteRegenMode(scan_task, DAQmx_Val_AllowRegen);
			err = DAQmxSetSampTimingType(scan_task, DAQmx_Val_SampleClock);
			err = DAQmxCfgSampClkTiming(scan_task, NULL, dac_rate, DAQmx_Val_Rising, DAQmx_Val_ContSamps, NULL);
			if (err != 0)
			{
				printf("DAQmx failed to program the task:\n");
				char* buf = new char[512];
				DAQmxGetErrorString(err, buf, 512);
				printf(buf);
				printf("\n");
				delete[] buf;
				return err;
			}
		}
		return err;
	}

	int daq_close()
	{
		err = DAQmxClearTask(scan_task);
		delete[] concatenated_scansig;
		printf("NI DAQ interface closed.\n");
		return err;
	}

	// These interact with both NI-IMAQ and NI-DAQmx APIs

	int start_scan()
	{
			err = DAQmxCfgOutputBuffer(scan_task, scansig_n);
			err = DAQmxWriteAnalogF64(scan_task, scansig_n, false, 1000, DAQmx_Val_GroupByChannel, concatenated_scansig, &samples_written, NULL);
			err = DAQmxStartTask(scan_task);
			if (err == 0)
			{
				err = imgSessionStartAcquisition(session_id);
				if (err == 0)
				{
					printf("Started scan!\n");
					return 0;
				}
			}
			return err;
	}

	int stop_scan()
	{
		err = imgSessionStopAcquisition(session_id);
		err = DAQmxStopTask(scan_task);
		if (err == 0)
		{
			return 0;
		}
		else
		{
			return err;
		}
	}

	int examine_buffer(uint16_t** raw_frame_addr, int frame_index)
	{
		err = imgSessionExamineBuffer2(session_id, frame_index, &examined_number, (void**)raw_frame_addr);
		if (err == 0)
		{
			return examined_number;
		}
		else
		{
			*raw_frame_addr = NULL;
			return err;
		}
	}

	int release_buffer()
	{
		return imgSessionReleaseBuffer(session_id);
	}

	int set_scan_pattern(ScanPattern* pattern)
	{
		// Assign buffers for scan pattern
		if (pattern->n != scansig_n)  // If buffer size needs to change
		{
			scansig_n = pattern->n;  // Set property to new n
			delete[] concatenated_scansig;
			concatenated_scansig = new float64[4 * pattern->n];
		}
		memcpy(concatenated_scansig + 0, pattern->x, sizeof(float64) * pattern->n);
		memcpy(concatenated_scansig + pattern->n, pattern->y, sizeof(float64) * pattern->n);
		memcpy(concatenated_scansig + 2 * pattern->n, pattern->line_trigger, sizeof(float64) * pattern->n);
		memcpy(concatenated_scansig + 3 * pattern->n, pattern->frame_trigger, sizeof(float64) * pattern->n);

		bool32 is_it = false;
		DAQmxIsTaskDone(scan_task, &is_it);
		if (!is_it)  // Only buffer the samples now if the task is running. Otherwise DAQmxCfgOutputBuffer and DAQmxWriteAnalogF64 are called on start_scan.
		{
			err = DAQmxCfgOutputBuffer(scan_task, scansig_n);
			err = DAQmxWriteAnalogF64(scan_task, scansig_n, false, 1000, DAQmx_Val_GroupByChannel, concatenated_scansig, &samples_written, NULL);
		}
		return err;
	}
}
