#pragma once

#include "niimaq.h"
#include "NIDAQmx.h"

// TODO remove. For testing
#include <stdlib.h>
#include <time.h>

// Functional interface for OCT frame grabbing and scanning using National Instruments libraries

int err;  // Error state of hardware interface

// IMAQ

char cameraName[256];  // Name used to open the IMAQ session
SESSION_ID session_id;  // IMAQ session ID
BUFLIST_ID buflist_id;  // IMAQ buflist ID
INTERFACE_ID interface_id;  // IMAQ interface ID
TaskHandle scan_task;  // NI-DAQ task handle

int32_t acqWinWidth;  // A-line size
int32_t acqWinHeight;  // Number of A-lines per buffer
int bytesPerPixel;
int32_t bufferSize;  // Equivalent to acqWinWidth * acqWinHeight * bytesPerPixel
int numberOfBuffers = 0;  // Number of IMAQ ring buffer elements
int bufferNumber;  // Cumulative acquired IMAQ ring buffer element

uInt32 examined_number;  // Cumulative number of the currently examined IMAQ ring buffer element

Int8** imaq_buffers;  // Ring buffer elements managed by IMAQ

uint16_t** buffers = NULL;  // Ring buffer elements allocated manually

// NI-DAQ

std::vector<float64> concatenated_scansig;  // Pointer to buffer of scansignals appended end to end

int dac_rate = -1;  // The output rate of the DAC used to drive the scan pattern
int line_rate = -1;  // The rate of the line trigger. A multiple of the DAC rate.
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
	int sample_rate;
	int line_rate;

	ScanPattern(double* x, double* y, double* line_trigger, int n, int sample_rate, int line_rate)
	{
		this->x = new double[n];
		memcpy(this->x, x, sizeof(double) * n);

		this->y = new double[n];
		memcpy(this->y, y, sizeof(double) * n);

		this->line_trigger = new double[n];
		memcpy(this->line_trigger, line_trigger, sizeof(double) * n);

		this->n = n;
		this->sample_rate = sample_rate;
		this->line_rate = line_rate;
	}

	~ScanPattern()
	{
		delete[] this->x;
		delete[] this->y;
		delete[] this->line_trigger;
	}

};


inline void print_daqmx_error_msg(int error_code)
{
	if (error_code != 0)
	{
		char* buf = new char[512];
		DAQmxGetErrorString(error_code, buf, 512);
		printf(buf);
		printf("\n");
		delete[] buf;
	}
	else
	{
		printf("No error.\n");
	}
}


inline int configure_scan_timing(ScanPattern* pattern)
{
	err = DAQmxSetWriteRegenMode(scan_task, DAQmx_Val_AllowRegen);
	err = DAQmxSetSampTimingType(scan_task, DAQmx_Val_SampleClock);
	err = DAQmxCfgSampClkTiming(scan_task, NULL, (double)pattern->sample_rate, DAQmx_Val_Rising, DAQmx_Val_ContSamps, pattern->n);

	return err;
}

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
		strcpy_s(cameraName, camera_name); // Save the camera name in case it needs to be reused. If this isn't a copy, arguments from Python will become undefined in async environment
		return err;
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

	int imaq_close()
	{
		err = imgClose(session_id, TRUE);
		err = imgClose(interface_id, TRUE);
		imaq_buffer_cleanup();
		return err;
	}

	int setup_buffers(int aline_size, int32_t number_of_alines, int number_of_buffers)
	{
		if (buffers != NULL)  // Reopen the session if there are already buffers present.
		{
			err = imaq_close();
			err = imaq_open(cameraName);
		}

		if (err != 0)
		{
			return err;
		}

		err = imgSetAttribute2(session_id, IMG_ATTR_ACQWINDOW_TOP, 0);
		err = imgSetAttribute2(session_id, IMG_ATTR_ACQWINDOW_LEFT, 0);
		err = imgSetAttribute2(session_id, IMG_ATTR_ACQWINDOW_HEIGHT, number_of_alines);
		err = imgSetAttribute2(session_id, IMG_ATTR_ACQWINDOW_WIDTH, aline_size);
		err = imgSetAttribute2(session_id, IMG_ATTR_ROWPIXELS, aline_size);
		err = imgSetAttribute2(session_id, IMG_ATTR_BYTESPERPIXEL, 2);

		// Confirm the change by getting the attributes
		err = imgGetAttribute(session_id, IMG_ATTR_ROI_WIDTH, &acqWinWidth);
		err = imgGetAttribute(session_id, IMG_ATTR_ROI_HEIGHT, &acqWinHeight);
		err = imgGetAttribute(session_id, IMG_ATTR_BYTESPERPIXEL, &bytesPerPixel);

		bufferSize = acqWinWidth * acqWinHeight * bytesPerPixel;

		buffers = new uint16_t*[number_of_buffers];
		for (int i = 0; i < number_of_buffers; i++)
		{
			buffers[i] = new uint16_t[aline_size * number_of_alines];
			memset(buffers[i], 0, aline_size * number_of_alines * sizeof(uint16_t));
		}
		if (number_of_buffers > 0)
		{
			err = imgRingSetup(session_id, number_of_buffers, (void**)buffers, 0, 0);
		}
		numberOfBuffers = number_of_buffers;
		return err;
	}

	int daq_open(
		const char* aoScanX,
		const char* aoScanY,
		const char* aoLineTrigger
	)
	{
		
		dac_rate = -1;
		line_rate = -1;
		scansig_n = 0;
		samples_written = 0;
		
		err = DAQmxCreateTask("scan", &scan_task);
		err = DAQmxCreateAOVoltageChan(scan_task, aoScanX, "", -10, 10, DAQmx_Val_Volts, NULL);
		err = DAQmxCreateAOVoltageChan(scan_task, aoScanY, "", -10, 10, DAQmx_Val_Volts, NULL);
		err = DAQmxCreateAOVoltageChan(scan_task, aoLineTrigger, "", -10, 10, DAQmx_Val_Volts, NULL);
		return err;
	}

	int daq_close()
	{
		err = DAQmxClearTask(scan_task);
		printf("NI DAQ interface closed.\n");
		return err;
	}

	// These interact with both NI-IMAQ and NI-DAQmx APIs

	int start_scan()
	{
		err = imgSessionStartAcquisition(session_id);
		if (err == 0)
		{
			err = DAQmxCfgOutputBuffer(scan_task, scansig_n);
			err = DAQmxWriteAnalogF64(scan_task, scansig_n, false, 1000, DAQmx_Val_GroupByChannel, &concatenated_scansig[0], &samples_written, NULL);
			err = DAQmxStartTask(scan_task);
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
			/*
			If session is not reopened before a second buffer setup, erroneous pointers will be returned without an error code,
			resulting in access violations. This code checks for that error case.
			for (int i = 0; i < numberOfBuffers; i++)
			{
				if (buffers[i] == *raw_frame_addr)
				{
					return examined_number;
				}
			}
			printf("Grabbed %p which was not in buffer list!\n", *raw_frame_addr);
			*/

		}
		*raw_frame_addr = NULL;
		return err;
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
			concatenated_scansig.reserve(3 * pattern->n);
		}

		memcpy(&concatenated_scansig[0] + 0, pattern->x, sizeof(float64) * pattern->n);
		memcpy(&concatenated_scansig[0] + pattern->n, pattern->y, sizeof(float64) * pattern->n);
		memcpy(&concatenated_scansig[0] + 2 * pattern->n, pattern->line_trigger, sizeof(float64) * pattern->n);

		bool32 is_it = false;
		DAQmxIsTaskDone(scan_task, &is_it);
		if (!is_it)  // Only buffer the samples now if the task is running. Otherwise DAQmxCfgOutputBuffer and DAQmxWriteAnalogF64 are called on start_scan.
		{
			// If rate or samples in scan has changed, need to reconfigure the timing of the output task
			if ((pattern->sample_rate != dac_rate) || (pattern->n != scansig_n) || (pattern->line_rate != line_rate))
			{
				err = DAQmxStopTask(scan_task);

				err = configure_scan_timing(pattern);
				
				err = DAQmxStartTask(scan_task);
			}
			else
			{
				printf("DAC rate is unchanged: %i\n", dac_rate);
			}
			err = DAQmxCfgOutputBuffer(scan_task, scansig_n);
			err = DAQmxWriteAnalogF64(scan_task, scansig_n, false, 1000, DAQmx_Val_GroupByChannel, &concatenated_scansig[0], &samples_written, NULL);
		}
		else
		{
			if ((pattern->sample_rate != dac_rate) || (pattern->n != scansig_n) || (pattern->line_rate != line_rate))
			{
				err = configure_scan_timing(pattern);
			}
			else
			{
				printf("DAC rate is unchanged: %i\n", dac_rate);
			}
		}
		printf("Changed DAC output rate from %i to %i\n", dac_rate, pattern->sample_rate);
		printf("Changed line trigger rate from %i to %i\n", line_rate, pattern->line_rate);
		printf("Changed pattern length from %i to %i\n", scansig_n, pattern->n);
		scansig_n = pattern->n;  // Set property to new n
		dac_rate = pattern->sample_rate;
		line_rate = pattern->line_rate;
		return err;
	}
}
