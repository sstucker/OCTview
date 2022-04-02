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

// NI-DAQ

int dac_rate;  // The output rate of the DAC used to drive the scan pattern
double* concatenated_scansig;  // Pointer to buffer of scansignals appended end to end
int scansig_n; // Number of samples in each of the 4 scan signals
int32* samples_written;  // Returned by NI DAQ after samples are written


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
		err = imgMemUnlock(buflist_id);
		for (int i = 0; i < numberOfBuffers; i++)
		{
			if (imaq_buffers[i] != NULL)
			{
				err = imgDisposeBuffer(imaq_buffers[i]);
			}
		}
		err = imgDisposeBufList(buflist_id, FALSE);
		return err;
	}

	int setup_buffers(int aline_size, int number_of_alines, int number_of_buffers)
	{
		if (numberOfBuffers > 0)  // Clean up previous buffers
		{
			err = imaq_buffer_cleanup();
		}
		err = imgSetAttribute2(session_id, IMG_ATTR_ACQWINDOW_TOP, 0);
		err = imgSetAttribute2(session_id, IMG_ATTR_ACQWINDOW_LEFT, 0);
		err = imgSetAttribute2(session_id, IMG_ATTR_ACQWINDOW_HEIGHT, number_of_alines);
		err = imgSetAttribute2(session_id, IMG_ATTR_ACQWINDOW_WIDTH, aline_size);
		if (err != 0)
		{
			return err;
		}
		// Confirm the change by getting the attributes
		err = imgGetAttribute(session_id, IMG_ATTR_ROI_WIDTH, &acqWinWidth);
		err = imgGetAttribute(session_id, IMG_ATTR_ROI_HEIGHT, &acqWinHeight);
		err = imgGetAttribute(session_id, IMG_ATTR_BYTESPERPIXEL, &bytesPerPixel);
		if (err != 0)
		{
			return err;
		}

		bufferSize = acqWinWidth * acqWinHeight * bytesPerPixel;

		numberOfBuffers = number_of_buffers;

		err = imgCreateBufList(number_of_buffers, &buflist_id);
		imaq_buffers = new Int8 *[number_of_buffers];

		int bufCmd;
		for (int i = 0; i < numberOfBuffers; i++)
		{
			err = imgCreateBuffer(session_id, FALSE, bufferSize, (void**)&imaq_buffers[i]);
			if (err != 0)
			{
				return err;
			}
			err = imgSetBufferElement2(buflist_id, i, IMG_BUFF_ADDRESS, imaq_buffers[i]);
			if (err != 0)
			{
				return err;
			}
			err = imgSetBufferElement2(buflist_id, i, IMG_BUFF_SIZE, bufferSize);
			if (err != 0)
			{
				return err;
			}
			bufCmd = (i == (number_of_buffers - 1)) ? IMG_CMD_LOOP : IMG_CMD_NEXT;
			if (err != 0)
			{
				return err;
			}
			err = imgSetBufferElement2(buflist_id, i, IMG_BUFF_COMMAND, bufCmd);
			if (err != 0)
			{
				return err;
			}
		}
		err = imgMemLock(buflist_id);
		if (err != 0)
		{
			return err;
		}
		err = imgSessionConfigure(session_id, buflist_id);
		if (err != 0)
		{
			return err;
		}
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
		printf("NI IMAQ interface closed.\n");
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
		printf("Creating DAQmx channels:\n");
		printf("X Channel ID: %s\n", aoScanX);
		printf("Y Channel ID: %s\n", aoScanY);
		printf("Line Trigger Channel ID: %s\n", aoLineTrigger);
		printf("Frame Trigger Channel ID: %s\n", aoFrameTrigger);
		printf("Start Trigger Channel ID: %s\n", aoStartTrigger);
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
			err = DAQmxSetWriteRegenMode(scan_task, DAQmx_Val_AllowRegen);
			err = DAQmxSetSampTimingType(scan_task, DAQmx_Val_SampleClock);
			err = DAQmxCfgSampClkTiming(scan_task, NULL, 152000, DAQmx_Val_Rising, DAQmx_Val_ContSamps, NULL);
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
			dac_rate = 152000;
		}
		return err;
	}

	int daq_close()
	{
		err = DAQmxClearTask(scan_task);
		delete[] concatenated_scansig;
		delete[] samples_written;
		printf("NI DAQ interface closed.\n");
		return err;
	}

	// These interact with both NI-IMAQ and NI-DAQmx APIs

	int start_scan()
	{
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

	int examine_buffer(uint16_t* raw_frame_addr, int frame_index)
	{
		err = imgSessionExamineBuffer2(session_id, frame_index, &examined_number, (void**)&raw_frame_addr);
		if (err == 0 && raw_frame_addr != NULL)
		{
			return examined_number;
		}
		else
		{
			raw_frame_addr = NULL;
			frame_index = -1;
			return err;
		}
	}

	int release_buffer()
	{
		return imgSessionReleaseBuffer(session_id);
	}

	int set_scan_pattern(double* x, double* y, double* linetrigger, double* frametrigger, int n, int pattern_sample_rate)
	{
		// Assign buffers for scan pattern

		delete[] samples_written;
		delete[] concatenated_scansig;
		samples_written = new int32[4];
		concatenated_scansig = new double[4 * n];
		memcpy(concatenated_scansig + 0, x, sizeof(double) * n);
		memcpy(concatenated_scansig + n, y, sizeof(double) * n);
		memcpy(concatenated_scansig + 2 * n, linetrigger, sizeof(double) * n);
		memcpy(concatenated_scansig + 3 * n, frametrigger, sizeof(double) * n);

		if (n != scansig_n)  // If buffer size needs to change
		{
			bool32 is_it = false;
			DAQmxIsTaskDone(scan_task, &is_it);
			if (!is_it)  // If task is running, need to stop, change buffer size, and restart it
			{
				err = DAQmxStopTask(scan_task);
				// err = DAQmxCfgOutputBuffer(scan_task, n);
				err = DAQmxWriteAnalogF64(scan_task, n, false, 1000, DAQmx_Val_GroupByChannel, concatenated_scansig, samples_written, NULL);
				err = DAQmxStartTask(scan_task);

			}
			else  // If task isn't running, just buffer new samples without starting
			{
				// err = DAQmxCfgOutputBuffer(scan_task, n);
				err = DAQmxWriteAnalogF64(scan_task, n, false, 1000, DAQmx_Val_GroupByChannel, concatenated_scansig, samples_written, NULL);
			}
		}
		else
		{
			err = DAQmxWriteAnalogF64(scan_task, n, false, 1000, DAQmx_Val_GroupByChannel, concatenated_scansig, samples_written, NULL);
		}

		if (err != 0)
		{
			printf("DAQmx failed to set scan signals:\n");
			char* buf = new char[512];
			DAQmxGetErrorString(err, buf, 512);
			printf(buf);
			printf("\n");
			delete[] buf;
		}

		scansig_n = n;  // Set property to new n
		return err;
	}
}
