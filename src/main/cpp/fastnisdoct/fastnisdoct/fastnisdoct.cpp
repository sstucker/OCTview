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
	STATE_ACQUIRING = 5,
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
	const char* ao_st_ch;
	int aline_size;
	int64_t alines_in_scan;
	int64_t alines_in_image;
	bool* image_mask;
	int64_t alines_per_buffer;
	int64_t alines_per_bline;
	int n_aline_repeat;
	int n_bline_repeat;
	int frames_to_buffer;
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
	bool save_processed;
};

spsc_bounded_queue_t<StateMsg> msg_queue(32);
std::unique_ptr<AlineProcessingPool> aline_proc_pool;  // Thread pool manager class. Responsible for background subtraction, apodization, interpolation, FFT

bool image_configured;  // Image buffers are allocated
bool processing_configured;  // Plans for interpolation and FFT are ready
bool scan_defined;  // DAC is primed for output

std::atomic_bool scan_interrupt_;  // Event that is armed by STOP_SCAN which forces the scanner out of a long acquisition cycle

std::atomic_int state;

int32_t alines_in_scan;  // Number of A-lines in the scan of a single frame prior to the discarding of non image-forming A-lines.
int32_t alines_in_image;  // Number of A-lines included in the image. alines_in_scan - alines_in_image A-lines are discarded.

int64_t preprocessed_alines_size;  // Total number of voxels in the entire frame of raw spectra
int64_t processed_alines_size;  // Number of complex-valued voxels in the frame after A-line processing has been carried out. Includes repeated A-lines, B-lines and frames.
int64_t processed_frame_size;  // Number of complex-valued voxels in the frame after inter A-line processing has been carried out: No repeats.

int32_t alines_per_buffer;  // Number of A-lines in each IMAQ buffer. If less than the total number of A-lines per frame, buffers will be concatenated to form a frame
int32_t buffers_per_frame;  // If > 0, IMAQ buffers will be copied into the processed A-lines buffer
int32_t alines_per_bline; // Number of A-lines which make up a B-line of the image. Used to divide processing labor.

std::unique_ptr<CircAcqBuffer<uint16_t>> spectral_image_buffer;  // Spectral frames are copied to this buffer for export.
std::unique_ptr<CircAcqBuffer<fftwf_complex>> processed_image_buffer;  // Spatial frames are written into this buffer for export.
int frames_to_buffer;  // Amount of buffer memory to allocate per the size of a frame

// Main loop should only spend time copying to the display buffers if the client is ready 
std::atomic_bool image_display_buffer_refresh;  // If True, grab returns -1 but main loop copies new frame in and flips the bit
std::unique_ptr<fftwf_complex[]> image_display_buffer;

std::atomic_bool spectrum_display_buffer_refresh;
std::unique_ptr<float[]> spectrum_display_buffer;

// I do not trust std containers for the large arrays
std::unique_ptr<uint16_t[]> raw_frame_roi;  // Frame which the contents of IMAQ buffers are copied into prior to processing if buffers_per_frame > 1
std::unique_ptr<uint16_t[]> raw_frame_roi_new;
std::vector<bool> discard_mask;  // Bitmask which reduces number_of_alines_buffered to number_of_alines. Intended to remove unwanted A-lines exposed during flyback, etc.

std::vector<std::vector<std::tuple<int, int>>> roi_cpy_map; // Variable number of (offset, start) for each buffer. Predetermined and used to optimize copying the ROI.

std::vector<float> background_spectrum;  // Subtracted from each spectrum.
std::vector<float> background_spectrum_new;

std::vector<float> apodization_window;  // Multiplied by each spectrum.

std::vector<uint16_t> aline_stamp_buffer; // A-line stamps are copied here. For debugging and latency monitoring

int32_t cumulative_buffer_number;  // Number of buffers acquired by IMAQ
int32_t cumulative_frame_number;  // Number of frames acquired by main

int aline_size;  // The number of voxels in each spectral A-line; the number of spectrometer bins
int roi_offset;  // The offset from the top of the spatial A-line, in voxels, from which to begin the cropped image
int roi_size;  // The number of voxels of the spatial A-line to include in the cropped image

int n_aline_repeat;  // The number of A-lines which are repeated in the image.
int n_bline_repeat;  // The number of B-lines which are repeated in the image.
int n_frame_avg;  // The number of frames which are to be averaged in the image.

RepeatProcessingType a_rpt_proc_flag;  // Processing to apply to repeated A-lines
RepeatProcessingType b_rpt_proc_flag;  // Processing to apply to repeated B-lines
bool subtract_background;  // If true, the average spectrum of the previous frame is subtracted from the subsequent frame.
bool interp;  // If true, first order linear interpolation is used to approximate a linear-in-wavelength spectrum.
double interpdk;  // Coefficient of first order linear-in-wavelength approximation.

float frame_processing_period;  // Time taken to process latest frame

bool saving_processed;
FileStreamWorker<uint16_t> spectral_frame_streamer;
FileStreamWorker<fftwf_complex> processed_frame_streamer;

// Set all module data to initial values
inline void init_fastnisdoct()
{
	image_configured = false;
	processing_configured = false;
	scan_defined = false;

	std::atomic_init(&scan_interrupt_, false);
	std::atomic_init(&state, STATE_UNOPENED);

	std::atomic_init(&image_display_buffer_refresh, true);
	std::atomic_init(&spectrum_display_buffer_refresh, true);

	alines_in_scan = 0;
	alines_in_image = 0;

	preprocessed_alines_size = 0;
	processed_alines_size = 0;
	processed_frame_size = 0;

	alines_per_buffer = 0;
	buffers_per_frame = 0;
	alines_per_bline = 0;

	frames_to_buffer = 0;

	cumulative_buffer_number = 0;
	cumulative_frame_number = 0;

	aline_size = 0;
	roi_offset = 0;
	roi_size = 0;

	n_aline_repeat = 1;
	n_bline_repeat = 1;
	n_frame_avg = 1;

	a_rpt_proc_flag = REPEAT_PROCESSING_NONE;
	b_rpt_proc_flag = REPEAT_PROCESSING_NONE;
	subtract_background = false;
	interp = false;
	interpdk = 0.0;

	frame_processing_period = 0.0;
}


inline void stop_acquisition()
{
	printf("fastnisdoct: stopping acquisition.\n");
	processed_frame_streamer.stop();
	spectral_frame_streamer.stop();
	ni::drive_start_trigger_low();
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
		aline_proc_pool = std::make_unique<AlineProcessingPool>(aline_size, alines_in_image, roi_offset, roi_size, true);
		printf("fastnisdoct: Processing pool created for the first time.\n");
		return;
	}
	else
	{
		if ((aline_proc_pool->aline_size != aline_size) || (aline_proc_pool->number_of_alines != alines_in_image) ||
			(aline_proc_pool->roi_offset != roi_offset) || (aline_proc_pool->roi_size != roi_size))
		{
			aline_proc_pool = std::make_unique<AlineProcessingPool>(aline_size, alines_in_image, roi_offset, roi_size, true);
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
						// printf("Found block at %i with size %i\n", offset * aline_size, size * aline_size);
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
			if (current_state == STATE_ACQUIRING)
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
					background_spectrum.reserve(msg.aline_size);
					background_spectrum_new.reserve(msg.aline_size);
					std::fill(background_spectrum.begin(), background_spectrum.end(), 0.0);
					std::fill(background_spectrum_new.begin(), background_spectrum_new.end(), 0.0);
				}
				else
				{
					printf("Allocating A-line-sized processing buffers with size %i\n", msg.aline_size);
				}

				if (alines_in_scan != msg.alines_in_scan)
				{
					aline_stamp_buffer.reserve(msg.alines_in_scan);
				}

				// -- Set up NI image buffers --------------------------------------------------------------------------
				if ((aline_size != msg.aline_size) || (alines_per_buffer != msg.alines_per_buffer) || (alines_in_scan != msg.alines_in_scan) || (alines_in_image != msg.alines_in_image) || (frames_to_buffer != msg.frames_to_buffer))  // If acq buffer size has changed
				{
					buffers_per_frame = msg.alines_in_scan / msg.alines_per_buffer;
					frames_to_buffer = msg.frames_to_buffer;
					if (ni::setup_buffers(msg.aline_size, msg.alines_per_buffer, buffers_per_frame * frames_to_buffer) == 0)
					{
						printf("fastnisdoct: %i buffers allocated with %i A-lines per buffer, %i buffers per frame.\n", buffers_per_frame * frames_to_buffer, msg.alines_per_buffer, buffers_per_frame);
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

					spectrum_display_buffer = std::make_unique<float[]>(aline_size);
					memset(spectrum_display_buffer.get(), 0.0, aline_size * sizeof(float));
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
					raw_frame_roi = std::make_unique<uint16_t[]>(preprocessed_alines_size);
					raw_frame_roi_new = std::make_unique<uint16_t[]>(preprocessed_alines_size);
					memset(raw_frame_roi.get(), 0, preprocessed_alines_size * sizeof(uint16_t));
					memset(raw_frame_roi_new.get(), 0, preprocessed_alines_size * sizeof(uint16_t));
					spectral_image_buffer = std::make_unique<CircAcqBuffer<uint16_t>>(frames_to_buffer, preprocessed_alines_size);
				}
				
				// Allocate rings
				if (msg.roi_size * msg.alines_in_image != processed_alines_size)
				{
					processed_alines_size = msg.roi_size * msg.alines_in_image;
					processed_image_buffer = std::make_unique<CircAcqBuffer<fftwf_complex>>(frames_to_buffer, processed_alines_size);
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

				image_display_buffer = std::make_unique<fftwf_complex[]>(processed_frame_size);

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
			auto current_state = state.load();
			if (current_state == STATE_ACQUIRING)
			{
				printf("fastnisdoct: Cannot configure processing during acquisition.\n");
			}
			else
			{
				processing_configured = false;

				subtract_background = msg.subtract_background;
				printf("fastnisdoct: Background subtraction %i\n", subtract_background);
				interp = msg.interp;
				interpdk = msg.interpdk;
				n_frame_avg = msg.n_frame_avg;

				// Apod window signal gets copied to module-managed buffer (allocated when image is configured)
				apodization_window.reserve(msg.aline_size);
				std::fill(apodization_window.begin(), apodization_window.end(), 1.0);
				apodization_window.insert(apodization_window.end(), msg.apod_window, msg.apod_window + msg.aline_size);
				delete[] msg.apod_window;
				
				// Can only attempt to set up processing pool if image params have been defined already. Otherwise pool gets set up then
				if (image_configured)
				{
					set_up_processing_pool();
					processing_configured = true;
				}
				// Transition to READY if necessary
				if (ready_to_scan() && state == STATE_OPEN)
				{
					state.store(STATE_READY);
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
			if (state.load() == STATE_ACQUIRING)
			{
				stop_acquisition();
			}
			if (state.load() == STATE_SCANNING) 
			{
				stop_scanning();
			}
		}
		else if (msg.flag & MSG_START_ACQUISITION)
		{
			printf("fastnisdoct: MSG_START_ACQUISITION received\n");
			if (state.load() == STATE_SCANNING)
			{
				if (msg.save_processed)
				{
					saving_processed = true;
					if (msg.n_frames_to_acquire > -1)
					{
						processed_frame_streamer.start(msg.file_name, msg.max_gb, (FileStreamType)msg.file_type, processed_image_buffer.get(), roi_size * alines_in_image, msg.n_frames_to_acquire);
					}
					else
					{
						processed_frame_streamer.start(msg.file_name, msg.max_gb, (FileStreamType)msg.file_type, processed_image_buffer.get(), roi_size * alines_in_image);
					}
				}
				else
				{
					saving_processed = false;
					if (msg.n_frames_to_acquire > -1)
					{
						spectral_frame_streamer.start(msg.file_name, msg.max_gb, (FileStreamType)msg.file_type, spectral_image_buffer.get(), preprocessed_alines_size, msg.n_frames_to_acquire);
					}
					else
					{
						spectral_frame_streamer.start(msg.file_name, msg.max_gb, (FileStreamType)msg.file_type, spectral_image_buffer.get(), preprocessed_alines_size);
					}
				}
				ni::drive_start_trigger_high();
				state.store(STATE_ACQUIRING);
				delete[] msg.file_name;
			}
		}
		else if (msg.flag & MSG_STOP_ACQUISITION)
		{
			printf("fastnisdoct: MSG_STOP_ACQUISITION received\n");
			if (state.load() == STATE_ACQUIRING)
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

	bool scanning_successfully;

	LARGE_INTEGER frequency;
	LARGE_INTEGER start;
	LARGE_INTEGER end;

	QueryPerformanceFrequency(&frequency);

	uint16_t* locked_out_addr = NULL;
	fftwf_complex* processed_alines_addr = NULL;

	state.store(STATE_OPEN);
	while (main_running)
	{
		if (state.load() == STATE_ACQUIRING && processed_frame_streamer.is_streaming() == false && spectral_frame_streamer.is_streaming() == false)  // If acquisition has finished, stop
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

			processed_alines_addr = processed_image_buffer->lock_out_head();  // Lock out the export ring element we are writing to. This is what gets written to disk by a Writer

			QueryPerformanceCounter(&start);  // Time the frame processing to make sure we should be able to keep up

			// Send async job to AlineProcessingPool unless we have not grabbed a frame yet
			if (cumulative_frame_number > 0)
			{
				aline_proc_pool->submit(processed_alines_addr, (uint16_t*)raw_frame_roi.get(), interp, interpdk, &apodization_window[0], &background_spectrum[0]);
			}

			// Set background spectrum to zero. We sum to it while holding each buffer
			std::fill(background_spectrum_new.begin(), background_spectrum_new.end(), 0.0);

			// Collect IMAQ buffers until whole frame is acquired
			int i_buf = 0;
			int i_img = 0;
			int buffer_copy_p = 0;
			while (i_buf < buffers_per_frame)
			{

				if (scan_interrupt_.load())
				{
					scan_interrupt_.store(false);
					scanning_successfully = false;
					break;
				}

				// Lock out frame with IMAQ function
				int examined = ni::examine_buffer(&locked_out_addr, cumulative_buffer_number);
				if (examined > -1)
				{
					
					if (examined != cumulative_buffer_number)
					{
						printf("Expected %i, got %i... Dropped frames.\n", cumulative_buffer_number, examined);
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
							memcpy(raw_frame_roi.get() + buffer_copy_p, locked_out_addr + std::get<0>(roi_cpy_map[i_buf][j]), std::get<1>(roi_cpy_map[i_buf][j]) * sizeof(uint16_t));
 							buffer_copy_p += std::get<1>(roi_cpy_map[i_buf][j]);
							// printf("Copying %i voxels (%i A-lines) from buffer %i, offset %i to buffer at position %i (%i A-lines) of %i\n", std::get<1>(roi_cpy_map[i_buf][j]), std::get<1>(roi_cpy_map[i_buf][j]) / aline_size, i_buf, std::get<0>(roi_cpy_map[i_buf][j]), buffer_copy_p, buffer_copy_p / aline_size, alines_in_image * aline_size);
						}
					}
					else
					{
						memcpy(raw_frame_roi.get() + buffer_copy_p, locked_out_addr, alines_per_buffer * aline_size * sizeof(uint16_t));
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
					scanning_successfully = true;
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
					scanning_successfully = false;
					break;
				}

			}  // Buffers per frame

			if (!saving_processed && current_state == STATE_ACQUIRING)
			{
				uint16_t* spectral_dst = spectral_image_buffer->lock_out_head();
				memcpy(spectral_dst, raw_frame_roi.get(), preprocessed_alines_size * sizeof(uint16_t));
				spectral_image_buffer->release_head();
			}

			// Only process a frame if we need it for export or if it is time to display one
			if (scanning_successfully) 
			{
				// Sum for average background spectrum (to be used with next scan)
				if (subtract_background)
				{
					for (int i = 0; i < alines_in_image; i++)
					{
						for (int j = 0; j < aline_size; j++)
						{
							background_spectrum_new[j] += raw_frame_roi_new.get()[aline_size * i + j];
						}
					}
					// Normalize the background spectrum
					float norm = 1.0 / alines_in_image;
					for (int j = 0; j < aline_size; j++)
					{
						background_spectrum_new[j] *= norm;
					}

					// Pointer swap new background buffer in
					std::swap(background_spectrum, background_spectrum_new);
				}
				else
				{
					std::fill(background_spectrum.begin(), background_spectrum.end(), 0.0);
				}

				// Pointer swap new grab buffer buffer in
				std::swap(raw_frame_roi, raw_frame_roi_new);

				// Buffer a spectrum for output to GUI
				if (spectrum_display_buffer_refresh.load())
				{
					for (int i = 0; i < aline_size; i++)
					{
						spectrum_display_buffer[i] = raw_frame_roi.get()[i] - background_spectrum[i] * (int)(subtract_background);  // Always grab from beginning of the buffer 
					}
					spectrum_display_buffer_refresh.store(false);
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

					fftwf_complex r;
					int alines_per_bline_now;  // A-lines per B-line following A-line repeat processing
					if (a_rpt_proc_flag == REPEAT_PROCESSING_MEAN && n_aline_repeat > 1)  // A-line averaging
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
										// printf("A-line averaging: Getting sum from image[%i, %i]\n", b * alines_per_bline + x * n_aline_repeat + k, z);
										r[0] += processed_alines_addr[(b * alines_per_bline + x * n_aline_repeat + k) * roi_size + z][0];
										r[1] += processed_alines_addr[(b * alines_per_bline + x * n_aline_repeat + k) * roi_size + z][1];
									}
									processed_alines_addr[(b * alines_per_bline_now + x) * roi_size + z][0] = r[0] / n_aline_repeat;
									processed_alines_addr[(b * alines_per_bline_now + x) * roi_size + z][1] = r[1] / n_aline_repeat;
									// printf("A-line averaging: Assigning sum to image[%i, %i]\n", b * alines_per_bline_now + x, z);
								}
							}
						}
					}
					else
					{
						// If A-line repeats are left in the frame for B-line processing
						alines_per_bline_now = alines_per_bline;
					}

					if (b_rpt_proc_flag > REPEAT_PROCESSING_NONE && n_bline_repeat > 1)
					{
						// printf("For each b in %i\n", alines_in_image / alines_per_bline / n_bline_repeat);
						// printf("For each A-line per bline %i\n", alines_per_bline_now);
						for (int b = 0; b < alines_in_image / alines_per_bline; b++)  // For each B-line in the (potentially A-line averaged) preprocessed frames
						{
							for (int x = 0; x < alines_per_bline_now / n_bline_repeat; x++)  // For each element of each B-line (minus repeats)
							{

								if (b_rpt_proc_flag == REPEAT_PROCESSING_DIFF && n_bline_repeat == 2)
								{
									//printf("B-line differencing: setting [%i] equal to [%i] - [%i]\n", (b * alines_per_bline_now / 2 + x), (b * alines_per_bline_now + x), (b * alines_per_bline_now + x + alines_per_bline_now / 2));
									for (int z = 0; z < roi_size; z++)
									{
										processed_alines_addr[(b * alines_per_bline_now / 2 + x) * roi_size + z][0] = std::abs(processed_alines_addr[(b * alines_per_bline_now + x) * roi_size + z][0] - processed_alines_addr[(b * alines_per_bline_now + x + alines_per_bline_now / 2) * roi_size + z][0]);
										processed_alines_addr[(b * alines_per_bline_now / 2 + x) * roi_size + z][1] = std::abs(processed_alines_addr[(b * alines_per_bline_now + x) * roi_size + z][1] - processed_alines_addr[(b * alines_per_bline_now + x + alines_per_bline_now / 2) * roi_size + z][1]);
									}
								}
								else  // REPEAT_PROCESSING_AVERAGING
								{
									float norm = 1.0 / n_bline_repeat;

									/*
									for (int k = 0; k < n_bline_repeat; k++)
									{
										printf("Summing from %i\n", (b * alines_per_bline_now + x + (alines_per_bline_now / n_bline_repeat) * k));
									}
									printf("Adding to %i\n", (b * (alines_per_bline_now / n_bline_repeat) + x));
									*/
									for (int z = 0; z < roi_size; z++)
									{
										r[0] = 0.0;
										r[1] = 0.0;
										for (int k = 0; k < n_bline_repeat; k++)
										{
											r[0] += processed_alines_addr[(b * alines_per_bline_now + x + (alines_per_bline_now / n_bline_repeat) * k) * roi_size + z][0];
											r[1] += processed_alines_addr[(b * alines_per_bline_now + x + (alines_per_bline_now / n_bline_repeat) * k) * roi_size + z][1];
										}
										processed_alines_addr[(b * (alines_per_bline_now / n_bline_repeat) + x) * roi_size + z][0] = r[0] * norm;
										processed_alines_addr[(b * (alines_per_bline_now / n_bline_repeat) + x) * roi_size + z][1] = r[1] * norm;
									}
								}
							}
						}
					}

					// Perform frame averaging

					// Buffer an image for output to GUI
					if (image_display_buffer_refresh.load())
					{
						memcpy(image_display_buffer.get(), processed_alines_addr, processed_frame_size * sizeof(fftwf_complex));
						image_display_buffer_refresh.store(false);
					}

					QueryPerformanceCounter(&end);
					frame_processing_period = (float)(end.QuadPart - start.QuadPart) / frequency.QuadPart;

					if (cumulative_frame_number % 256 == 0)
					{
						printf("Processed frame %i elapsed %f, %f Hz, \n", cumulative_frame_number - 1, frame_processing_period, 1.0 / frame_processing_period);
					}
				}
				cumulative_frame_number++;
			}
			processed_image_buffer->release_head();
		}
		// printf("fastnisdoct: Main loop running. State %i\n", state.load());
		// fflush(stdout);
	}
	if (state.load() == STATE_ACQUIRING)
	{
		stop_acquisition();
	}
	if (state.load() == STATE_SCANNING)
	{
		stop_scanning();
	}
	printf("Exiting main\n");
	fflush(stdout);
}


extern "C"  // DLL interface. Functions should enqueue messages or interact with the main_t.
{

	__declspec(dllexport) void nisdoct_open(
		const char* cam_name,
		const char* ao_x_ch,
		const char* ao_y_ch,
		const char* ao_lt_ch,
		const char* ao_st_ch
	)
	{
		if (main_running.load())
		{
			printf("fastnisdoct: Can't open controller, already open\n");
			return;
		}
		printf("fastnisdoct: Opening NI hardware interface:\n");
		printf("fastnisdoct: Camera ID: %s\n", cam_name);
		printf("fastnisdoct: X channel ID: %s\n", ao_x_ch);
		printf("fastnisdoct: Y channel ID: %s\n", ao_y_ch);
		printf("fastnisdoct: Line trig channel ID: %s\n", ao_lt_ch);
		printf("fastnisdoct: Start trigger channel ID: %s\n", ao_st_ch);

		// If you don't use these strings here, dynamically put them somewhere until you do--their values are undefined once Python scope changes or if enqueued

		if (ni::imaq_open(cam_name) == 0)
		{
			printf("fastnisdoct: NI IMAQ interface opened.\n");
			if (ni::daq_open(ao_x_ch, ao_y_ch, ao_lt_ch, ao_st_ch) == 0)
			{
				printf("fastnisdoct: NI DAQmx interface opened.\n");
				init_fastnisdoct();
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
		if (main_running.load())
		{
			main_running = false;
			main_t.join();
			printf("Joined main thread.\n");
			StateMsg msg;
			while (msg_queue.dequeue(msg)) {}  // Empty the message queue
			printf("Emptied the message queue.\n");
			if (ni::daq_close() == 0 && ni::imaq_close() == 0)
			{
				printf("NI IMAQ and NI DAQmx interfaces closed.\n");
			}
			else
			{
				printf("Failed to close NI IMAQ and NI DAQmx interfaces.\n");
				ni::print_error_msg();
			}
		}
		else
		{
			printf("Can't close: fastnisdoct not running!\n");
		}
	}

	__declspec(dllexport) void nisdoct_configure_image(
		int aline_size,
		int64_t alines_in_scan,
		bool* image_mask,
		int64_t alines_in_image,
		int64_t alines_per_bline,
		int64_t alines_per_buffer,
		int frames_to_buffer,
		int n_aline_repeat,
		int n_bline_repeat,
		RepeatProcessingType a_rpt_proc_flag,
		RepeatProcessingType b_rpt_proc_flag,
		int roi_offset,
		int roi_size,
		double* x_scan_signal,
		double* y_scan_signal,
		double* line_trigger_scan_signal,
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
		msg.frames_to_buffer = frames_to_buffer;
		msg.n_aline_repeat = n_aline_repeat;
		msg.n_bline_repeat = n_bline_repeat;
		msg.a_rpt_proc_flag = a_rpt_proc_flag;
		msg.b_rpt_proc_flag = b_rpt_proc_flag;
		msg.roi_offset = roi_offset;
		msg.roi_size = roi_size;
		msg.scanpattern = new ScanPattern(x_scan_signal, y_scan_signal, line_trigger_scan_signal, n_samples_per_signal, signal_output_rate, line_rate);
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
		scan_interrupt_.store(true);
		StateMsg msg;
		msg.flag = MSG_STOP_SCAN;
		msg_queue.enqueue(msg);
	}

	__declspec(dllexport) void nisdoct_start_bin_acquisition(
		const char* file,
		float max_gb,
		int n_frames_to_acquire,
		bool save_processed
	)
	{
		StateMsg msg;
		msg.file_name = new char[512];
		memcpy((void*)msg.file_name, file, strlen(file) * sizeof(char) + 1);
		msg.max_gb = max_gb;
		msg.file_type = FSTREAM_TYPE_RAW;
		msg.n_frames_to_acquire = n_frames_to_acquire;
		msg.save_processed = save_processed;
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
		return (state.load() == STATE_ACQUIRING);
	}

	__declspec(dllexport) int nisdoct_grab_frame(fftwf_complex* dst)
	{
		auto current_state = state.load();
		if (current_state == STATE_SCANNING || current_state == STATE_ACQUIRING)
		{
			if (image_display_buffer_refresh.load())
			{
				return -1;  // No new frame ready
			}
			else
			{
				memcpy(dst, image_display_buffer.get(), processed_frame_size * sizeof(fftwf_complex));
				image_display_buffer_refresh.store(true);
				return 0;
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
		if (current_state == STATE_SCANNING || current_state == STATE_ACQUIRING)
		{
			if (spectrum_display_buffer_refresh.load())
			{
				return -1;
			}
			else
			{
				memcpy(dst, spectrum_display_buffer.get(), aline_size * sizeof(float));
				spectrum_display_buffer_refresh.store(true);
				return 0;
			}
		}
		else
		{
			return -1;
		}
	}

}


