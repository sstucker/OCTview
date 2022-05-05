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
#include <cstring>
#include <stdlib.h>
#include <windows.h>
#include <complex>

#include "spscqueue.h"
#include "AlineProcessingPool.h"
#include "CircAcqBuffer.h"
#include "FileStreamWorker.h"
#include "ni.h"

#define IDLE_SLEEP_MS 10


enum OCTState
{
	STATE_UNOPENED = 1,
	STATE_OPEN = 2,
	STATE_READY = 3,
	STATE_SCANNING = 4,
	STATE_ACQUIRIING = 5,
	STATE_BUSY = 6,
	STATE_ERROR = 7

};
DEFINE_ENUM_FLAG_OPERATORS(OCTState);


enum RepeatProcessingType
{
	REPEAT_PROCESSING_NONE = 0,
	REPEAT_PROCESSING_MEAN = 1,
	REPEAT_PROCESSING_DIFF = 2
};
DEFINE_ENUM_FLAG_OPERATORS(RepeatProcessingType);


// msgs are passed into the main thread
#define MSG_CONFIGURE_IMAGE		  static_cast<int>( 1 << 0 )
#define MSG_CONFIGURE_PROCESSING  static_cast<int>( 1 << 1 )
#define MSG_START_SCAN			  static_cast<int>( 1 << 2 )
#define MSG_STOP_SCAN             static_cast<int>( 1 << 3 )
#define MSG_START_ACQUISITION     static_cast<int>( 1 << 4 )
#define MSG_STOP_ACQUISITION      static_cast<int>( 1 << 5 )

struct StateMsg {
	
	int flag;
	
	const char* cam_name;
	const char* ao_x_ch;
	const char* ao_y_ch;
	const char* ao_lt_ch;
	const char* ao_ft_ch;
	const char* ao_st_ch;
	int aline_size;
	int64_t alines_in_scan;
	int64_t alines_in_image;
	bool* image_mask;
	int64_t alines_per_buffer;
	int64_t alines_per_bline;
	int n_aline_repeat;
	int n_bline_repeat;
	int number_of_buffers;
	int roi_offset;
	int roi_size;
	bool subtract_background;
	bool interp;
	double interpdk;
	float* apod_window;
	RepeatProcessingType a_rpt_proc_flag;
	RepeatProcessingType b_rpt_proc_flag;
	int n_frame_avg;
	ScanPattern* scanpattern;
	const char* file_name;
	int file_type;
	float max_gb;
	int n_frames_to_acquire;
};

spsc_bounded_queue_t<StateMsg> msg_queue(32);
AlineProcessingPool* aline_proc_pool = NULL;  // Thread pool manager class. Responsible for background subtraction, apodization, interpolation, FFT

bool image_configured = false;  // Image buffers are allocated
bool processing_configured = false;  // Plans for interpolation and FFT are ready
bool scan_defined = false;  // DAC is primed for output

std::atomic_int state = STATE_UNOPENED;

int32_t alines_in_scan = 0;  // Number of A-lines in the scan of a single frame prior to the discarding of non image-forming A-lines.
int32_t alines_in_image = 0;  // Number of A-lines included in the image. alines_in_scan - alines_in_image A-lines are discarded.

int64_t preprocessed_alines_size = 0;  // Total number of voxels in the entire frame of raw spectra
int64_t processed_alines_size = 0;  // Number of complex-valued voxels in the frame after A-line processing has been carried out. Includes repeated A-lines, B-lines and frames.
int64_t processed_frame_size = 0;  // Number of complex-valued voxels in the frame after inter A-line processing has been carried out: No repeats.

int32_t alines_per_buffer = 0;  // Number of A-lines in each IMAQ buffer. If less than the total number of A-lines per frame, buffers will be concatenated to form a frame
int32_t buffers_per_frame = 0;  // If > 0, IMAQ buffers will be copied into the processed A-lines buffer
int32_t alines_per_bline = 0; // Number of A-lines which make up a B-line of the image. Used to divide processing labor.

CircAcqBuffer<fftwf_complex>* processed_image_buffer = NULL;  // Ring buffer which frames are written to prior to export.
int frames_to_buffer = 0;  // Amount of buffer memory to allocate per the size of a frame

// TODO replace with CircAcqBuffers or otherwise preallocate display buffers

// Small queues, main loop should only spend time copying to the display buffers if the GUI is ready 
spsc_bounded_queue_t<fftwf_complex*> image_display_queue(4);  // Queue of pointers to images for display. Copy to client buffer and then delete after dequeue.
spsc_bounded_queue_t<float*> spectrum_display_queue(4);  // Queue of pointers to spectra for display. Copy to client buffer and then delete after dequeue.

uint16_t* raw_frame_roi = NULL;  // Frame which the contents of IMAQ buffers are copied into prior to processing if buffers_per_frame > 1
uint16_t* raw_frame_roi_new = NULL;
bool* discard_mask = NULL;  // Bitmask which reduces number_of_alines_buffered to number_of_alines. Intended to remove unwanted A-lines exposed during flyback, etc.

std::vector<std::vector<std::tuple<int, int>>> roi_cpy_map; // Variable number of (offset, start) for each buffer. Predetermined and used to optimize copying the ROI.

float* background_spectrum = NULL;  // Subtracted from each spectrum.
float* background_spectrum_new = NULL;

float* apodization_window = NULL;  // Multiplied by each spectrum.

uint16_t* aline_stamp_buffer = NULL; // A-line stamps are copied here. For debugging and latency monitoring

int32_t cumulative_buffer_number = 0;  // Number of buffers acquired by IMAQ
int32_t cumulative_frame_number = 0;  // Number of frames acquired by main

int aline_size = 0;  // The number of voxels in each spectral A-line; the number of spectrometer bins
int roi_offset = 0;  // The offset from the top of the spatial A-line, in voxels, from which to begin the cropped image
int roi_size = 0;  // The number of voxels of the spatial A-line to include in the cropped image

int n_aline_repeat = 1;  // The number of A-lines which are repeated in the image.
int n_bline_repeat = 1;  // The number of B-lines which are repeated in the image.
int n_frame_avg = 1;  // The number of frames which are to be averaged in the image.

RepeatProcessingType a_rpt_proc_flag = REPEAT_PROCESSING_NONE;  // Processing to apply to repeated A-lines
RepeatProcessingType b_rpt_proc_flag = REPEAT_PROCESSING_NONE;  // Processing to apply to repeated B-lines
bool subtract_background = false;  // If true, the average spectrum of the previous frame is subtracted from the subsequent frame.
bool interp = false;  // If true, first order linear interpolation is used to approximate a linear-in-wavelength spectrum.
double interpdk = 0.0;  // Coefficient of first order linear-in-wavelength approximation.


inline void stop_acquisition()
{
	stop_streaming();
	state.store(STATE_SCANNING);
}

inline void start_scanning()
{
	aline_proc_pool->start();
	if (ni::start_scan() == 0)
	{
		printf("fastnisdoct: Scanning!\n");
		state.store(STATE_SCANNING);
	}
	else
	{
		aline_proc_pool->terminate();
		printf("fastnisdoct: Failed to start scanning.");
		ni::print_error_msg();
	}
}

inline void stop_scanning()
{
	if (ni::stop_scan() == 0)
	{
		printf("fastnisdoct: Stopping scan!\n");
		aline_proc_pool->terminate();
		// Empty out the display queues
		fftwf_complex* ip;
		while (image_display_queue.dequeue(ip))
		{
			fftwf_free(ip);
		}
		float* sp;
		while (spectrum_display_queue.dequeue(sp))
		{
			delete[] sp;
		}
		state.store(STATE_READY);
	}
	else
	{
		printf("fastnisdoct: Failed to stop scanning.\n");
		ni::print_error_msg();
	}
}

// Avoid setting up the processing workers if it is unecessary
inline void set_up_processing_pool()
{
	if (aline_proc_pool == NULL)
	{
		delete aline_proc_pool;
		aline_proc_pool = new AlineProcessingPool(aline_size, alines_in_image, roi_offset, roi_size, true);
		printf("fastnisdoct: Processing pool created for the first time.\n");
		return;
	}
	else
	{
		if ((aline_proc_pool->aline_size != aline_size) || (aline_proc_pool->number_of_alines != alines_in_image) ||
			(aline_proc_pool->roi_offset != roi_offset) || (aline_proc_pool->roi_size != -roi_size))
		{
			delete aline_proc_pool;
			aline_proc_pool = new AlineProcessingPool(aline_size, alines_in_image, roi_offset, roi_size, true);
			printf("fastnisdoct: Processing pool recreated.\n");
			return;
		}
	}
	printf("fastnisdoct: Processing pool does not need to be recreated.\n");
}


// Iterate over image_mask and reduce it to a vector containing copy offsets and sizes per each acquisition buffer.
inline void plan_acq_copy(bool* image_mask)
{
	roi_cpy_map.clear();
	if (alines_in_scan > alines_in_image)
	{
		int offset = -1;
		int size = 0;
		int i_frame = 0;
		for (int i = 0; i < buffers_per_frame; i++)
		{
			std::vector<std::tuple<int, int>> blocks_in_buffer;
			for (int j = 0; j < alines_per_buffer; j++)
			{
				if (size == 0)  // If not counting a copy block
				{
					if (image_mask[i_frame])  // Enter new block
					{
						size = 1;
						offset = j;
					}
				}
				else  // If in a block
				{
					if (image_mask[i_frame])
					{
						size++;
					}
					else  // Block has ended
					{
						printf("Found block at %i with size %i\n", offset * aline_size, size * aline_size);
						blocks_in_buffer.push_back(std::tuple<int, int>{ offset * aline_size, size * aline_size });
						offset = -1;
						size = 0;
					}
				}
				i_frame++;
			}
			roi_cpy_map.push_back(blocks_in_buffer);
		}
	}
}

inline bool ready_to_scan()
{
	return (image_configured && processing_configured && scan_defined);
}

inline void recv_msg()
{
	StateMsg msg;
	if (msg_queue.dequeue(msg))
	{
		if (msg.flag & MSG_CONFIGURE_IMAGE)
		{
			printf("fastnisdoct: MSG_CONFIGURE_IMAGE received\n");
			bool restart = false;  // Set true if scan should be started after configuration
			auto current_state = state.load();
			if (current_state == STATE_ACQUIRIING)
			{
				printf("Cannot configure image during acquisition!\n");
				return;
			}
			else if (current_state == STATE_SCANNING)
			{
				restart = true;
				stop_scanning();
				current_state = state.load();  // Get our state again (will be READY if stop_scanning was successful)
			}
			if (current_state == STATE_READY || current_state == STATE_OPEN)
			{
				state.store(STATE_OPEN);  // While in this block, we are not ready to scan.
				image_configured = false;
				processing_configured = false;

				if (aline_size != msg.aline_size)  // If A-line size has changed
				{
					printf("Allocating A-line-sized processing buffers with size %i\n", msg.aline_size);

					// Allocate processing buffers
					delete[] apodization_window;
					apodization_window = new float[msg.aline_size];
					memset(apodization_window, 1, msg.aline_size * sizeof(float));

					delete[] background_spectrum;
					delete[] background_spectrum_new;
					background_spectrum = new float[msg.aline_size];
					background_spectrum_new = new float[msg.aline_size];
					memset(background_spectrum, 0, msg.aline_size * sizeof(float));
					memset(background_spectrum_new, 0, msg.aline_size * sizeof(float));
				}
				else
				{
					printf("Allocating A-line-sized processing buffers with size %i\n", msg.aline_size);
				}

				if (alines_in_scan != msg.alines_in_scan)
				{
					delete[] aline_stamp_buffer;
					aline_stamp_buffer = new uint16_t[msg.alines_in_scan];
				}

				// -- Set up NI image buffers --------------------------------------------------------------------------
				if ((aline_size != msg.aline_size) || (alines_per_buffer != msg.alines_per_buffer) || (alines_in_scan != msg.alines_in_scan) || (alines_in_image != msg.alines_in_image))  // If acq buffer size has changed
				{
					buffers_per_frame = msg.alines_in_scan / msg.alines_per_buffer;
					if (ni::setup_buffers(msg.aline_size, msg.alines_per_buffer, frames_to_buffer * buffers_per_frame) == 0)
					{
						printf("fastnisdoct: %i buffers allocated.\n", frames_to_buffer * buffers_per_frame);
						cumulative_buffer_number = 0;
						cumulative_frame_number = 0;
						image_configured = true;
					}
					else
					{
						printf("fastnisdoct: Failed to allocate buffers.\n");
						ni::print_error_msg();
					}

					aline_size = msg.aline_size;
					alines_in_scan = msg.alines_in_scan;
					alines_in_image = msg.alines_in_image;
					printf("A-lines in scan: %i\n", alines_in_scan);
					printf("A-lines in image: %i\n", alines_in_image);
					alines_per_bline = msg.alines_per_bline;
					alines_per_buffer = msg.alines_per_buffer;
				}
				else
				{
					printf("fastnisdoct: Buffers did not change size!\n");
					image_configured = true;
				}

				// -- Allocate processing buffers if they have changed size --------------------------------------------------------------------------
				if (msg.aline_size * msg.alines_in_image != preprocessed_alines_size)
				{
					preprocessed_alines_size = msg.aline_size * msg.alines_in_image;
					delete[] raw_frame_roi;
					delete[] raw_frame_roi_new;
					raw_frame_roi = new uint16_t[preprocessed_alines_size];
					raw_frame_roi_new = new uint16_t[preprocessed_alines_size];
				}
				
				// Allocate rings
				if (msg.roi_size * msg.alines_in_image != processed_alines_size)
				{
					delete processed_image_buffer;
					processed_alines_size = msg.roi_size * msg.alines_in_image;
					processed_image_buffer = new CircAcqBuffer<fftwf_complex>(frames_to_buffer, processed_alines_size);
				}
				roi_offset = msg.roi_offset;
				roi_size = msg.roi_size;

				// Processed frame size is smaller than processed A-lines size if A-lines or frames are combined via averaging or differencing
				processed_frame_size = processed_alines_size;
				if (msg.a_rpt_proc_flag > REPEAT_PROCESSING_NONE)
				{
					processed_frame_size /= msg.n_aline_repeat;
				}
				if (msg.b_rpt_proc_flag > REPEAT_PROCESSING_NONE)
				{
					processed_frame_size /= msg.n_bline_repeat;
				}

				n_aline_repeat = msg.n_aline_repeat;
				n_bline_repeat = msg.n_bline_repeat;
				a_rpt_proc_flag = msg.a_rpt_proc_flag;
				b_rpt_proc_flag = msg.b_rpt_proc_flag;

				printf("fastnisdoct: Image configured: Number of A-lines: %i\n", alines_in_image);
				printf("fastnisdoct: Image configured: raw frame size: %i, processed frame size: %i\n", preprocessed_alines_size, processed_frame_size);

				// -- Predetermine indices to minimize copy operations  --------------------------------------------------------------------------
				plan_acq_copy(msg.image_mask);
				delete msg.image_mask;

				// -- Send scan signals to the DAC --------------------------------------------------------------------------
				scan_defined = false;
				if (ni::set_scan_pattern(msg.scanpattern) == 0)
				{
					scan_defined = true;
					printf("Buffered new scan pattern!\n");
				}
				else
				{
					printf("Error updating scan.\n");
					ni::print_error_msg();
				}
				delete msg.scanpattern;  // Free the pattern memory

				processing_configured = false;
				set_up_processing_pool();
				processing_configured = true;

				// -- Set back to READY --------------------------------------------------------------------------
				if (ready_to_scan() && state == STATE_OPEN)
				{
					state.store(STATE_READY);
				}
			}
			else
			{
				printf("fastnisdoct: Cannot configure image! Not OPEN or READY.\n");
			}
			if (restart)
			{
				start_scanning();
			}
		}
		else if (msg.flag & MSG_CONFIGURE_PROCESSING)
		{
			printf("fastnisdoct: MSG_CONFIGURE_PROCESSING received\n");
			if (image_configured)  // Image must be configured before processing can be configured
			{
				if (state.load() != STATE_SCANNING)  // If not scanning, update everything and reinitialize the A-line ThreadPool and processing buffers
				{
					state.store(STATE_OPEN);
					processing_configured = false;

					subtract_background = msg.subtract_background;
					interp = msg.interp;
					interpdk = msg.interpdk;
					n_frame_avg = msg.n_frame_avg;

					// Apod window signal gets copied to module-managed buffer (allocated when image is configured)
					memcpy(apodization_window, msg.apod_window, msg.aline_size * sizeof(float));
					delete[] msg.apod_window;
					
					set_up_processing_pool();

					processing_configured = true;
					if (ready_to_scan() && state == STATE_OPEN)
					{
						state.store(STATE_READY);
					}
				}
				else if (state.load() == STATE_ACQUIRIING)
				{
					printf("fastnisdoct: Cannot configure processing duration acquisition.\n");
				}
				else  // If SCANNING, make real-time adjustments to apod window, background subtraction option, interpolation
				{
					subtract_background = msg.subtract_background;
					interp = msg.interp;
					interpdk = msg.interpdk;

					// Apod window signal gets copied for safety(?)
					memcpy(apodization_window, msg.apod_window, aline_size * sizeof(float));
				}
			}
		}
		else if (msg.flag & MSG_START_SCAN)
		{
			printf("fastnisdoct: MSG_START_SCAN received\n");
			if (state.load() == STATE_READY)
			{
				// Initialize the AlineProcessingPool. This creates an FFTW plan and may take a long time.
				start_scanning();
			}
		}
		else if (msg.flag & MSG_STOP_SCAN)
		{
			printf("fastnisdoct: MSG_STOP_SCAN received\n");
			stop_scanning();
		}
		else if (msg.flag & MSG_START_ACQUISITION)
		{
			printf("fastnisdoct: MSG_START_ACQUISITION received\n");
			if (state.load() == STATE_SCANNING)
			{
				if (msg.n_frames_to_acquire > -1)
				{
					start_streaming(msg.file_name, msg.max_gb, (FileStreamType)msg.file_type, processed_image_buffer, roi_size, alines_in_image, msg.n_frames_to_acquire);
				}
				else
				{
					start_streaming(msg.file_name, msg.max_gb, (FileStreamType)msg.file_type, processed_image_buffer, roi_size, alines_in_image);
				}
				state.store(STATE_ACQUIRIING);
				delete[] msg.file_name;
			}
		}
		else if (msg.flag & MSG_STOP_ACQUISITION)
		{
			printf("fastnisdoct: MSG_STOP_ACQUISITION received\n");
			if (state.load() == STATE_ACQUIRIING)
			{
				stop_acquisition();
			}
		}
	}
}

std::atomic_bool main_running = false;
std::thread main_t;
void _main()
{
	// Initializations

	LARGE_INTEGER frequency;
	LARGE_INTEGER start;
	LARGE_INTEGER end;
	double interval;

	QueryPerformanceFrequency(&frequency);

	uint16_t* locked_out_addr = NULL;
	fftwf_complex* processed_alines_addr = NULL;

	state.store(STATE_OPEN);
	while (main_running)
	{
		if (state.load() == STATE_ACQUIRIING && is_streaming() == false)  // If acquisition has finished, stop
		{
			state = STATE_SCANNING;
			stop_scanning();
		}
		recv_msg();
		auto current_state = state.load();
		if (current_state == STATE_UNOPENED || current_state == STATE_OPEN || current_state == STATE_READY)
		{
			Sleep(IDLE_SLEEP_MS);  // Block for awhile if not scanning
		}
		else if (current_state == STATE_ERROR)
		{
			// printf("fastnisdoct: Error!\n");
			return;
		}
		else  // if SCANNING or ACQUIRING
		{
			QueryPerformanceCounter(&start);  // Time the frame processing to make sure we should be able to keep up

			// Send async job to AlineProcessingPool unless we have not grabbed a frame yet
			if (cumulative_frame_number > 0)
			{
				processed_alines_addr = processed_image_buffer->lock_out_head();  // Lock out the export ring element we are writing to. This is what gets written to disk by a Writer
				aline_proc_pool->submit(processed_alines_addr, raw_frame_roi, interp, interpdk, apodization_window, background_spectrum);
			}

			// Set background spectrum to zero. We sum to it while holding each buffer
			memset(background_spectrum_new, 0, aline_size * sizeof(float));

			// Collect IMAQ buffers until whole frame is acquired
			int i_buf = 0;
			int i_img = 0;
			int buffer_copy_p = 0;
			while (i_buf < buffers_per_frame)
			{
				// Lock out frame with IMAQ function
				int examined = ni::examine_buffer(&locked_out_addr, cumulative_buffer_number);
				if (examined > -1)
				{
					
					if (examined != cumulative_buffer_number)
					{
						printf("Expected %i, got %i... Missed frames.\n", cumulative_buffer_number, examined);
						cumulative_buffer_number = examined;
					}

					for (int i = 0; i < alines_per_buffer; i++)
					{
						aline_stamp_buffer[i_buf * alines_per_buffer + i] = locked_out_addr[i * aline_size];
						locked_out_addr[i * aline_size] = 0;
					}
					
					// Copy buffer to frame
					if (alines_in_image != alines_in_scan)
					{
						for (int j = 0; j < roi_cpy_map[i_buf].size(); j++)
						{
							memcpy(raw_frame_roi + buffer_copy_p, locked_out_addr + std::get<0>(roi_cpy_map[i_buf][j]), std::get<1>(roi_cpy_map[i_buf][j]) * sizeof(uint16_t));
 							buffer_copy_p += std::get<1>(roi_cpy_map[i_buf][j]);
							// printf("Copying %i voxels (%i A-lines) from buffer %i, offset %i to buffer at position %i (%i A-lines) of %i\n", std::get<1>(roi_cpy_map[i_buf][j]), std::get<1>(roi_cpy_map[i_buf][j]) / aline_size, i_buf, std::get<0>(roi_cpy_map[i_buf][j]), buffer_copy_p, buffer_copy_p / aline_size, alines_in_image * aline_size);
						}
					}
					else
					{
						memcpy(raw_frame_roi + buffer_copy_p, locked_out_addr, alines_per_buffer * aline_size * sizeof(uint16_t));
						buffer_copy_p += i_buf * alines_per_buffer * aline_size;
					}

					if (ni::release_buffer() != 0)
					{
						printf("fastnisdoct: Failed to release buffer!\n");
						ni::print_error_msg();
					}

					cumulative_buffer_number += 1;
					i_buf++;
					// printf("Got frame %i of %i. Total frames: %i\n", i_buf, buffers_per_frame, cumulative_buffer_number);

				}
				else  // If frame not grabbed properly
				{
					printf("fastnisdoct: Error examining buffer %i.\n", cumulative_buffer_number);
					ni::print_error_msg();
					if (ni::release_buffer() != 0)
					{
						printf("fastnisdoct: Failed to release buffer!\n");
						ni::print_error_msg();
					}
				}

			}  // Buffers per frame

			// Sum for average background spectrum (to be used with next scan)
			if (subtract_background)
			{
				for (int i = 0; i < alines_in_image; i++)
				{
					for (int j = 0; j < aline_size; j++)
					{
						background_spectrum_new[j] += raw_frame_roi_new[aline_size * i + j];
					}
				}
			}

			if (subtract_background)
			{
				// Normalize the background spectrum
				float norm = 1.0 / alines_in_image;
				for (int j = 0; j < aline_size; j++)
				{
					background_spectrum_new[j] *= norm;
				}

				// Pointer swap new background buffer in
				float* tmp = background_spectrum;
				background_spectrum = background_spectrum_new;
				background_spectrum_new = tmp;
			}
			else
			{
				memset(background_spectrum, 0, aline_size * sizeof(float));
			}

			// Pointer swap new grab buffer buffer in
			uint16_t* tmp = raw_frame_roi;
			raw_frame_roi = raw_frame_roi_new;
			raw_frame_roi_new = tmp;

			// Buffer a spectrum for output to GUI
			if (!spectrum_display_queue.full())
			{
				float* spectrum_buffer = new float[aline_size];  // Will be freed by grab_frame or cleanup
				for (int i = 0; i < aline_size; i++)
				{
					spectrum_buffer[i] = raw_frame_roi[i] - background_spectrum[i];  // Always grab from beginning of the buffer 
				}
				spectrum_display_queue.enqueue(spectrum_buffer);
			}

			// If there is already a processed buffer
			if (cumulative_frame_number > 0)
			{
				// Wait for async job to finish. If the above is false, we haven't started one yet
				int spins = 0;
				while (!aline_proc_pool->is_finished())
				{
					spins += 1;
				}
				// printf("fastnisdoct: A-line processing pool finished with frame %i. Spun %i times.\n", cumulative_frame_number, spins);

				// printf("A-line averaging: %i/%i, B-line averaging: %i/%i, Frame averaging %i\n", n_aline_repeat, a_rpt_proc_flag, n_bline_repeat, b_rpt_proc_flag, n_frame_avg);

				fftwf_complex r;
				int alines_per_bline_now;  // A-lines per B-line following A-line repeat processing
				if (a_rpt_proc_flag > REPEAT_PROCESSING_NONE)  // A-line averaging
				{
					alines_per_bline_now = alines_per_bline / n_aline_repeat;
					for (int b = 0; b < alines_in_image / alines_per_bline; b++)  // For each B-line in the preprocessed frames
					{
						for (int x = 0; x < alines_per_bline_now; x++)  // For each A-line in the reduced B-line
						{
							for (int z = 0; z < roi_size; z++)
							{
								r[0] = 0;
								r[1] = 0;
								for (int k = 0; k < n_aline_repeat; k++)
								{
									// printf("Getting sum from image[%i, %i]\n", b * alines_per_bline + x * n_aline_repeat + k, z);
									r[0] += processed_alines_addr[(b * alines_per_bline + x * n_aline_repeat + k) * roi_size + z][0];
									r[1] += processed_alines_addr[(b * alines_per_bline + x * n_aline_repeat + k) * roi_size + z][1];
								}
								processed_alines_addr[(b * alines_per_bline_now + x) * roi_size + z][0] = r[0] / n_aline_repeat;
								processed_alines_addr[(b * alines_per_bline_now + x) * roi_size + z][1] = r[1] / n_aline_repeat;
								// printf("Assigning sum to image[%i, %i]\n", b * alines_per_bline_now + x, z);
							}
						}
					}
				}
				else
				{
					// If A-line repeats are left in the frame for B-line processing
					alines_per_bline_now = alines_per_bline;
				}
				if (b_rpt_proc_flag > REPEAT_PROCESSING_NONE)  // B-line averaging
				{
					for (int b = 0; b < alines_in_image / alines_per_bline / n_bline_repeat; b++)  // For each B-line in the (potentially A-line averaged) preprocessed frames
					{
						for (int x = 0; x < alines_per_bline_now; x++)  // For each element of each B-line
						{
							for (int z = 0; z < roi_size; z++)
							{
								r[0] = 0;
								r[1] = 0;
								for (int k = 0; k < n_bline_repeat; k++)
								{
									// printf("B-line averaging: Getting sum from image[%i, %i]\n", b * alines_per_bline_now + x + k * alines_per_bline_now, z);
									r[0] += processed_alines_addr[(b * alines_per_bline_now + x + k * alines_per_bline_now) * roi_size + z][0];
									r[1] += processed_alines_addr[(b * alines_per_bline_now + x + k * alines_per_bline_now) * roi_size + z][1];
								}
								processed_alines_addr[(b * alines_per_bline_now / n_bline_repeat + x) * roi_size + z][0] = r[0] / n_bline_repeat;
								processed_alines_addr[(b * alines_per_bline_now / n_bline_repeat + x) * roi_size + z][1] = r[1] / n_bline_repeat;
								// printf("B-line averaging: assigning sum to image[%i, %i]\n", b * alines_per_bline_now / n_bline_repeat + x, z);
							}
						}
					}
				}
				
				// Perform frame averaging

				// Buffer an image for output to GUI
				if (!image_display_queue.full())
				{
					fftwf_complex* image_buffer = fftwf_alloc_complex(processed_frame_size);  // Will be freed by grab_frame or cleanup
					memcpy(image_buffer, processed_alines_addr, processed_frame_size * sizeof(fftwf_complex));
					image_display_queue.enqueue(image_buffer);
				}

				QueryPerformanceCounter(&end);
				interval = (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;

				processed_image_buffer->release_head();

				printf("Processed frame %i elapsed %f, %f Hz, \n", cumulative_frame_number - 1, interval, 1.0 / interval);

			}

			cumulative_frame_number++;

		}
		// printf("fastnisdoct: Main loop running. State %i\n", state.load());
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
		const char* ao_st_ch,
		int number_of_buffers
	)
	{
		printf("fastnisdoct: Opening NI hardware interface:\n");
		printf("fastnisdoct: Camera ID: %s\n", cam_name);
		printf("fastnisdoct: X channel ID: %s\n", ao_x_ch);
		printf("fastnisdoct: Y channel ID: %s\n", ao_y_ch);
		printf("fastnisdoct: Line trig channel ID: %s\n", ao_lt_ch);
		printf("fastnisdoct: Frame trig channel ID: %s\n", ao_ft_ch);
		printf("fastnisdoct: Start trig channel ID: %s\n", ao_st_ch);

		// If you don't use these strings here, dynamically put them somewhere until you do--their values are undefined once Python scope changes

		if (ni::imaq_open(cam_name) == 0)
		{
			printf("fastnisdoct: NI IMAQ interface opened.\n");
			if (ni::daq_open(ao_x_ch, ao_y_ch, ao_lt_ch, ao_ft_ch, ao_st_ch) == 0)
			{
				printf("fastnisdoct: NI DAQmx interface opened.\n");
				frames_to_buffer = number_of_buffers;
				main_running = true;
				main_t = std::thread(&_main);
				return;
			}
			ni::print_error_msg();
		}
		else
		{
			printf("fastnisdoct: Failed to open IMAQ interface.\n");
			ni::print_error_msg();
		}
	}

	__declspec(dllexport) void nisdoct_close()
	{
		main_running = false;
		main_t.join();
		delete aline_proc_pool;
		if (ni::daq_close() == 0 && ni::imaq_close() == 0)
		{
			printf("NI IMAQ and NI DAQmx interfaces closed.\n");
		}
		else
		{
			printf("Failed to close NI IMAQ and NI DAQmx interfaces.\n");
			ni::print_error_msg();
		}
		delete[] background_spectrum;
		delete[] background_spectrum_new;
		delete processed_image_buffer;
		delete apodization_window;
		delete[] raw_frame_roi;
		delete[] raw_frame_roi_new;
	}

	__declspec(dllexport) void nisdoct_configure_image(
		int aline_size,
		int64_t alines_in_scan,
		bool* image_mask,
		int64_t alines_in_image,
		int64_t alines_per_bline,
		int64_t alines_per_buffer,
		int n_aline_repeat,
		int n_bline_repeat,
		RepeatProcessingType a_rpt_proc_flag,
		RepeatProcessingType b_rpt_proc_flag,
		int roi_offset,
		int roi_size,
		double* x_scan_signal,
		double* y_scan_signal,
		double* line_trigger_scan_signal,
		double* frame_trigger_scan_signal,
		int64_t n_samples_per_signal,
		int signal_output_rate,
		int line_rate
	)
	{
		StateMsg msg;
		msg.aline_size = aline_size;
		msg.alines_in_scan = alines_in_scan;
		if (alines_in_scan > alines_in_image)
		{
			msg.image_mask = new bool[alines_in_scan];
			memcpy(msg.image_mask, image_mask, alines_in_scan * sizeof(bool));
		}
		else
		{
			msg.image_mask = NULL;
		}
		msg.alines_in_image = alines_in_image;
		msg.alines_per_bline = alines_per_bline;
		msg.alines_per_buffer = alines_per_buffer;
		msg.n_aline_repeat = n_aline_repeat;
		msg.n_bline_repeat = n_bline_repeat;
		msg.a_rpt_proc_flag = a_rpt_proc_flag;
		msg.b_rpt_proc_flag = b_rpt_proc_flag;
		msg.roi_offset = roi_offset;
		msg.roi_size = roi_size;
		msg.scanpattern = new ScanPattern(
			x_scan_signal, y_scan_signal,
			line_trigger_scan_signal, frame_trigger_scan_signal,
			n_samples_per_signal, signal_output_rate, line_rate
		);

		msg.flag = MSG_CONFIGURE_IMAGE;
		msg_queue.enqueue(msg);
	}

	__declspec(dllexport) void nisdoct_configure_processing(
		bool subtract_background,
		bool interp,
		double interpdk,
		float* apod_window,
		int aline_size,
		int n_frame_avg
	)
	{
		StateMsg msg;
		msg.subtract_background = subtract_background;
		msg.interp = interp;
		msg.interpdk = interpdk;
		msg.aline_size = aline_size;
		msg.apod_window = new float[aline_size];
		memcpy(msg.apod_window, apod_window, aline_size * sizeof(float));  // Will be freed after copy into async buffer
		msg.n_frame_avg = n_frame_avg;
		msg.flag = MSG_CONFIGURE_PROCESSING;
		msg_queue.enqueue(msg);
	}

	__declspec(dllexport) void nisdoct_start_scan()
	{
		StateMsg msg;
		msg.flag = MSG_START_SCAN;
		msg_queue.enqueue(msg);
	}

	__declspec(dllexport) void nisdoct_stop_scan()
	{
		StateMsg msg;
		msg.flag = MSG_STOP_SCAN;
		msg_queue.enqueue(msg);
	}

	__declspec(dllexport) void nisdoct_start_raw_acquisition(
		const char* file,
		float max_gb,
		int n_frames_to_acquire
	)
	{
		StateMsg msg;
		msg.file_name = new char[512];
		memcpy((void*)msg.file_name, file, strlen(file) * sizeof(char) + 1);
		msg.max_gb = max_gb;
		msg.file_type = FSTREAM_TYPE_RAW;
		msg.n_frames_to_acquire = n_frames_to_acquire;
		msg.flag = MSG_START_ACQUISITION;
		msg_queue.enqueue(msg);
	}

	__declspec(dllexport) void nisdoct_stop_acquisition()
	{
		StateMsg msg;
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

	__declspec(dllexport) int nisdoct_grab_frame(fftwf_complex* dst)
	{
		auto current_state = state.load();
		if (current_state == STATE_SCANNING || current_state == STATE_ACQUIRIING)
		{
			fftwf_complex* f = NULL;
			if (image_display_queue.dequeue(f))
			{
				// Note you can access processed_frame_size here because we just checked state, but really this is undefined
				memcpy(dst, f, processed_frame_size * sizeof(fftwf_complex));
				fftwf_free(f);
				return 0;
			}
			else
			{
				return -1;
			}
		}
		else
		{
			return -1;
		}
	}

	__declspec(dllexport) int nisdoct_grab_spectrum(float* dst)
	{
		auto current_state = state.load();
		if (current_state == STATE_SCANNING || current_state == STATE_ACQUIRIING)
		{
			float* s = NULL;
			if (spectrum_display_queue.dequeue(s))
			{
				// Note you can PROBABLY access state data here because we just checked state, but really this is undefined
				memcpy(dst, s, aline_size * sizeof(float));
				delete[] s;
				return 0;
			}
			else
			{
				return -1;
			}
		}
		else
		{
			return -1;
		}
	}

}


