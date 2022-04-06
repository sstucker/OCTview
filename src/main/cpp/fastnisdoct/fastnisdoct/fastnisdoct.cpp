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

#include "spscqueue.h"
#include "AlineProcessingPool.h"
#include "CircAcqBuffer.h"
#include "ni.h"
#include <complex>

#define IDLE_SLEEP_MS 10

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
#define STATE_ERROR				  static_cast<int>( 6 )


struct StateMsg {
	
	int flag;
	
	const char* cam_name;
	const char* ao_x_ch; 
	const char* ao_y_ch;
	const char* ao_lt_ch;
	const char* ao_ft_ch;
	const char* ao_st_ch;
	int aline_size;
	int spatial_aline_size;
	int number_of_alines;
	int alines_per_b;
	int buffer_each_b;
	int aline_repeat;
	int bline_repeat;
	int number_of_buffers;
	int roi_offset;
	int roi_size;
	bool fft_enabled;
	bool subtract_background;
	bool interp;
	double interpdk;
	float* apod_window;
	int a_rpt_proc_flag;
	int b_rpt_proc_flag;
	int n_frame_avg;
	ScanPattern* scanpattern;
	const char* file;
	long long max_bytes;
	int n_frames_to_acquire;
};

inline void printf_state_data(StateMsg s)
{
	printf("camera name: %s\n", s.cam_name);
	printf("ao x galvo name: %s\n", s.ao_x_ch);
	printf("ao y galvo name: %s\n", s.ao_y_ch);
	printf("ao lt galvo name: %s\n", s.ao_lt_ch);
	printf("ao ft galvo name: %s\n", s.ao_ft_ch);
	printf("ao st galvo name: %s\n", s.ao_st_ch);
	printf("aline_size: %i\n", s.aline_size);
	printf("number_of_alines: %i\n", s.number_of_alines);
	printf("alines_per_b: %i\n", s.alines_per_b);
	printf("aline_repeat: %i\n", s.aline_repeat);
	printf("bline_repeat: %i\n", s.bline_repeat);
	printf("number_of_buffers: %i\n", s.number_of_buffers);
	printf("roi_offset: %i\n", s.roi_offset);
	printf("roi_size: %i\n", s.roi_size);
	printf("fft_enabled: %i\n", s.fft_enabled);
	printf("subtract_background: %i\n", s.subtract_background);
	printf("interp: %i\n", s.interp);
	printf("interpdk: %f\n", s.interpdk);
	printf("apod_window: <%p>\n", s.apod_window);
	printf("a_rpt_proc_flag: %i\n", s.a_rpt_proc_flag);
	printf("b_rpt_proc_flag: %i\n", s.b_rpt_proc_flag);
	printf("n_frame_avg: %i\n", s.n_frame_avg);
	printf("file: %s\n", s.file);
	printf("max_bytes (in GB): %i\n", s.max_bytes / (long long)1073741824);
	printf("n_frames_to_acquire: %i\n", s.n_frames_to_acquire);
}

spsc_bounded_queue_t<StateMsg> msg_queue(32);

AlineProcessingPool* aline_processing_pool = NULL;

bool image_configured = false;
bool processing_configured = false;
bool scan_defined = false;

std::atomic_int state = STATE_UNOPENED;
StateMsg state_data;

int raw_frame_size = 0;  // Number of elements in the raw frame
int processed_alines_size = 0;  // Number of elements in the frame after A-line processing has been carried out
int processed_frame_size = 0;  // Number of elements in the frame after inter A-line processing (frame processed) has been carried out

int alines_per_buffer = 0;  // Number of A-lines in each IMAQ buffer. If less than the total number of A-lines per frame, buffers will be concatenated to form a frame
int buffers_per_frame = 0;  // If > 0, IMAQ buffers will be copied into the processed A-lines buffer

CircAcqBuffer<fftwf_complex>* processed_alines_ring = NULL;
CircAcqBuffer<fftwf_complex>* processed_frame_ring = NULL;

spsc_bounded_queue_t<fftwf_complex*> image_display_queue(4); // Small queues, main loop should only spend time copying to the display buffers if the GUI is ready 
spsc_bounded_queue_t<float*> spectrum_display_queue(4);

float* background_spectrum = NULL;
float* background_spectrum_new = NULL;

float* apodization_window = NULL;

int cumulative_buffer_number = 0;  // Number of buffers acquired by IMAQ

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
			if (state.load() == STATE_READY || state.load() == STATE_OPEN)
			{
				state.store(STATE_OPEN);
				image_configured = false;
				processing_configured = false;

				state_data.aline_size = msg.aline_size;
				state_data.spatial_aline_size = msg.aline_size / 2 + 1;
				state_data.number_of_alines = msg.number_of_alines;
				state_data.alines_per_b = msg.alines_per_b;
				state_data.buffer_each_b = msg.buffer_each_b;
				state_data.aline_repeat = msg.aline_repeat; 
				state_data.bline_repeat = msg.bline_repeat;
				state_data.number_of_buffers = msg.number_of_buffers;
				state_data.roi_offset = msg.roi_offset;
				state_data.roi_size = msg.roi_size;

				printf("Buffering B-lines: %i\n", state_data.buffer_each_b);

				// printf_state_data(state_data);
				raw_frame_size = msg.aline_size * msg.number_of_alines;
				processed_alines_size = msg.roi_size * msg.number_of_alines;
				processed_frame_size = state_data.roi_size * ((state_data.number_of_alines / state_data.aline_repeat) / state_data.bline_repeat);
				printf("fastnisdoct: Image configured: Number of A-lines: %i\n", state_data.number_of_alines);
				printf("fastnisdoct: Image configured: raw frame size: %i, processed frame size: %i\n", raw_frame_size, processed_frame_size);

				float total_buffer_size = msg.number_of_buffers * ((sizeof(uint16_t) * raw_frame_size) + (sizeof(fftwf_complex) * processed_frame_size));
				
				delete processed_alines_ring;
				delete processed_frame_ring;

				// Allocate or reallocate rings
				processed_alines_ring = new CircAcqBuffer<fftwf_complex>(state_data.number_of_buffers, processed_alines_size);
				processed_frame_ring = new CircAcqBuffer<fftwf_complex>(state_data.number_of_buffers, processed_frame_size);

				// Allocate processing buffers
				delete[] apodization_window;
				apodization_window = new float[state_data.aline_size];
				memset(apodization_window, 1, state_data.aline_size * sizeof(float));
				
				delete[] background_spectrum;
				delete[] background_spectrum_new;
				background_spectrum = new float[msg.aline_size];
				background_spectrum_new = new float[msg.aline_size];
				memset(background_spectrum, 0, state_data.aline_size * sizeof(float));
				memset(background_spectrum_new, 0, state_data.aline_size * sizeof(float));

				if (state_data.buffer_each_b)
				{
					alines_per_buffer = state_data.alines_per_b;
				}
				else
				{
					alines_per_buffer = state_data.number_of_alines;
				}
				buffers_per_frame = state_data.number_of_alines / alines_per_buffer;
				printf("Buffers per frame: %i\n", buffers_per_frame);
				printf("A-lines per buffer: %i\n", alines_per_buffer);

				// Set up NI image buffers
				if (ni::setup_buffers(state_data.aline_size, alines_per_buffer, state_data.number_of_buffers) == 0)
				{
					printf("fastnisdoct: Buffers allocated.\n");
					cumulative_buffer_number = 0;
					image_configured = true;
					if (ready_to_scan() && state == STATE_OPEN)
					{
						state.store(STATE_READY);
					}
				}
				else
				{
					printf("fastnisdoct: Failed to allocate buffers.\n");
					ni::print_error_msg();
				}
			}
			else
			{
				printf("fastnisdoct: Cannot configure image! Not OPEN or READY.\n");
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

					state_data.fft_enabled = msg.fft_enabled;
					state_data.subtract_background = msg.subtract_background;
					state_data.interp = msg.interp;
					state_data.interpdk = msg.interpdk;
					
					// Apod window signal gets copied to module-managed buffer (allocated when image is configured)
					memcpy(apodization_window, msg.apod_window, msg.aline_size * sizeof(float));
					delete[] msg.apod_window;

					state_data.a_rpt_proc_flag = msg.a_rpt_proc_flag;
					state_data.b_rpt_proc_flag = msg.b_rpt_proc_flag;
					state_data.n_frame_avg = msg.n_frame_avg;

					// Initialize the AlineProcessingPool. This creates an FFTW plan and may take a long time.
					aline_processing_pool = new AlineProcessingPool(state_data.aline_size, alines_per_buffer, state_data.roi_offset, state_data.roi_size, state_data.fft_enabled);

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
					state_data.subtract_background = msg.subtract_background;
					state_data.interp = msg.interp;
					state_data.interpdk = msg.interpdk;

					// Apod window signal gets copied for safety(?)
					memcpy(apodization_window, msg.apod_window, state_data.aline_size * sizeof(float));
				}
			}
		}
		else if (msg.flag & MSG_SET_PATTERN)
		{
			printf("fastnisdoct: MSG_SET_PATTERN received... ");
			scan_defined = false;
			if (ni::set_scan_pattern(msg.scanpattern) == 0)
			{
				scan_defined = true;
				delete msg.scanpattern;  // Free the pattern memory
				printf("Buffered new scan pattern!\n");
				if (ready_to_scan() && state == STATE_OPEN)
				{
					state.store(STATE_READY);
				}
			}
			else
			{
				printf("Error updating scan.\n");
				ni::print_error_msg();
			}
		}
		else if (msg.flag & MSG_START_SCAN)
		{
			printf("fastnisdoct: MSG_START_SCAN received\n");
			if (state.load() == STATE_READY)
			{
				aline_processing_pool->start();
				if (ni::start_scan() == 0)
				{
					printf("fastnisdoct: Scanning!\n");
					state.store(STATE_SCANNING);
				}
				else
				{
					aline_processing_pool->terminate();
					printf("fastnisdoct: Failed to start scanning.");
					ni::print_error_msg();
				}
			}
		}
		else if (msg.flag & MSG_STOP_SCAN)
		{
			printf("fastnisdoct: MSG_STOP_SCAN received\n");
			if (ni::stop_scan() == 0)
			{
				if (state.load() == STATE_SCANNING)
				{
					aline_processing_pool->terminate();
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
			}
			else
			{
				printf("fastnisdoct: Failed to stop scanning.\n");
				ni::print_error_msg();
			}
		}
		else if (msg.flag & MSG_START_ACQUISITION)
		{
			printf("fastnisdoct: MSG_START_ACQUISITION received\n");
			if (state.load() == STATE_SCANNING)
			{
				state.store(STATE_ACQUIRIING);
			}
		}
		else if (msg.flag & MSG_STOP_ACQUISITION)
		{
			printf("fastnisdoct: MSG_STOP_ACQUISITION received\n");
			if (state.load() == STATE_ACQUIRIING)
			{
				state.store(STATE_SCANNING);
			}
		}
	}
	else
	{
		// printf("fastnisdoct: Queue was empty.\n");
		// ni::print_error_msg();
	}
}


std::atomic_bool main_running = false;
std::thread main_t;
void _main()
{
	// Initializations

	uint16_t* raw_frame_addr = NULL;
	fftwf_complex* processed_alines_addr = NULL;

	state.store(STATE_OPEN);
	while (main_running)
	{
		recv_msg();
		if (state.load() == STATE_UNOPENED || state.load() == STATE_OPEN || state.load() == STATE_READY)
		{
			Sleep(IDLE_SLEEP_MS);  // Block for awhile if not scanning
		}
		else if (state.load() == STATE_ERROR)
		{
			// printf("fastnisdoct: Error!\n");
			return;
		}
		else  // if SCANNING or ACQUIRING
		{

			processed_alines_addr = processed_alines_ring->lock_out_head();

			// Set background spectrum to zero before sum
			memset(background_spectrum_new, 0, state_data.aline_size * sizeof(float));

			for (int ibuf = 0; ibuf < buffers_per_frame; ibuf++)  // Collect and process IMAQ buffers until whole frame is acquired
			{
				printf("Grabbing buffer %i of %i\n", ibuf, buffers_per_frame);
				// Lock out frame with IMAQ function
				int examined = ni::examine_buffer(&raw_frame_addr, cumulative_buffer_number);
				// printf("fastnisdoct: Examined buffer %i\n", examined);
				if (examined > -1 && raw_frame_addr != NULL)
				{
					printf("fastnisdoct: stamp[0] = %i\n", raw_frame_addr[state_data.aline_size * 0]);
					// printf("fastnisdoct: stamp[-1] = %i\n", raw_frame_addr[state_data.aline_size * (state_data.number_of_alines - 1)]);

					for (int i = 0; i < alines_per_buffer; i++)
					{
						raw_frame_addr[i * state_data.aline_size] = 0;  // Zero all stamps
					}

					// Buffer a spectrum for output to GUI
					if (!spectrum_display_queue.full())
					{
						float* spectrum_buffer = new float[state_data.aline_size];  // Will be freed by grab_frame or cleanup
						for (int i = 0; i < state_data.aline_size; i++)
						{
							spectrum_buffer[i] = raw_frame_addr[i] - background_spectrum[i];  // Always grab from beginning of the buffer 
						}
						spectrum_display_queue.enqueue(spectrum_buffer);
					}

					// Send job to AlineProcessingPool
					aline_processing_pool->submit(processed_alines_addr + (ibuf * alines_per_buffer * state_data.roi_size), raw_frame_addr, state_data.interp, state_data.interpdk, apodization_window, background_spectrum);

					// Calculate average background spectrum (to be used with next scan)
					if (state_data.subtract_background)
					{
						for (int i = 0; i < alines_per_buffer; i++) 
						{
							for (int j = 0; j < state_data.aline_size; j++)
							{
								background_spectrum_new[j] += raw_frame_addr[state_data.aline_size * i + j];
							}
						}
					}
					// Wait for job to finish
					int spins = 0;
					while (!aline_processing_pool->is_finished())
					{
						spins += 1;
					}
					// printf("fastnisdoct: A-line processing pool finished. Spun %i times.\n", spins);

					cumulative_buffer_number += 1;
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
					break;
				}

				if (ni::release_buffer() != 0)
				{
					printf("fastnisdoct: Failed to release buffer!\n");
					ni::print_error_msg();
				}

			}  // Buffers per frame
			

			if (state_data.subtract_background)
			{
				// Normalize the background spectrum
				float norm = 1.0 / state_data.number_of_alines;
				for (int j = 0; j < state_data.aline_size; j++)
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
				memset(background_spectrum, 0, state_data.aline_size * sizeof(float));
			}

			// Buffer an image for output to GUI
			if (!image_display_queue.full())
			{
				fftwf_complex* image_buffer = fftwf_alloc_complex(processed_alines_size);  // Will be freed by grab_frame or cleanup
				memcpy(image_buffer, processed_alines_addr, processed_alines_size * sizeof(fftwf_complex));
				image_display_queue.enqueue(image_buffer);
			}

			processed_alines_ring->release_head();
			// Perform A-line averaging or differencing
			// Perform B-line averaging or differencing
			// Perform frame averaging

			// if (state.load() == STATE_ACQUIRING)
			//		enqueue the frame with the StreamWorker
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
		const char* ao_st_ch
	)
	{
		printf("fastnisdoct: Opening NI hardware interface:\n");
		printf("fastnisdoct: Camera ID: %s\n", cam_name);
		printf("fastnisdoct: X channel ID: %s\n", ao_x_ch);
		printf("fastnisdoct: Y channel ID: %s\n", ao_y_ch);
		printf("fastnisdoct: Line trig channel ID: %s\n", ao_lt_ch);
		printf("fastnisdoct: Frame trig channel ID: %s\n", ao_ft_ch);
		printf("fastnisdoct: Start trig channel ID: %s\n", ao_st_ch);

		// If you don't use these strings here, dynamically put them somewhere until you do--their values are undefined once Python scope exits

		if (ni::imaq_open(cam_name) == 0)
		{
			printf("fastnisdoct: NI IMAQ interface opened.\n");
			if (ni::daq_open(ao_x_ch, ao_y_ch, ao_lt_ch, ao_ft_ch, ao_st_ch) == 0)
			{
				printf("fastnisdoct: NI DAQmx interface opened.\n");
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
		delete aline_processing_pool;
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
		delete processed_alines_ring;
		delete processed_frame_ring;
		delete apodization_window;
	}

	__declspec(dllexport) void nisdoct_configure_image(
		int aline_size,
		int number_of_alines,
		int alines_per_b,
		bool buffer_each_b,
		int aline_repeat,
		int bline_repeat,
		int number_of_buffers,
		int roi_offset,
		int roi_size
	)
	{
		StateMsg msg;
		msg.aline_size = aline_size;
		msg.number_of_alines = number_of_alines;
		msg.alines_per_b = alines_per_b;
		msg.buffer_each_b = buffer_each_b;
		msg.aline_repeat = aline_repeat;
		msg.bline_repeat = bline_repeat;
		msg.number_of_buffers = number_of_buffers;
		msg.roi_offset = roi_offset;
		msg.roi_size = roi_size;
		msg.flag = MSG_CONFIGURE_IMAGE;
		msg_queue.enqueue(msg);
	}

	__declspec(dllexport) void nisdoct_configure_processing(
		bool fft_enabled,
		bool subtract_background,
		bool interp,
		double interpdk,
		float* apod_window,
		int aline_size,
		int a_rpt_proc_flag,
		int b_rpt_proc_flag,
		int n_frame_avg
	)
	{
		StateMsg msg;
		msg.fft_enabled = fft_enabled;
		msg.subtract_background = subtract_background;
		msg.interp = interp;
		msg.interpdk = interpdk;
		msg.aline_size = aline_size;
		msg.apod_window = new float[aline_size];
		memcpy(msg.apod_window, apod_window, aline_size * sizeof(float));  // Will be freed after copy into async buffer
		msg.a_rpt_proc_flag = a_rpt_proc_flag;
		msg.b_rpt_proc_flag = b_rpt_proc_flag;
		msg.n_frame_avg = n_frame_avg;
		msg.flag = MSG_CONFIGURE_PROCESSING;
		msg_queue.enqueue(msg);
	}

	__declspec(dllexport) void nisdoct_set_pattern(
		double* x,
		double* y,
		double* line_trigger,
		double* frame_trigger,
		int n_samples,
		int rate
	)
	{
		StateMsg msg;
		msg.scanpattern = new ScanPattern(x, y, line_trigger, frame_trigger, n_samples, rate);  // Will be freed after dequeue and copy into hardware buffer
		msg.flag = MSG_SET_PATTERN;
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

	__declspec(dllexport) void nisdoct_start_acquisition(
		const char* file,
		long long max_bytes,
		int n_frames_to_acquire
	)
	{
		StateMsg msg;
		msg.file = file;
		msg.max_bytes = max_bytes;
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
			fftwf_complex* f;
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
			float* s;
			if (spectrum_display_queue.dequeue(s))
			{
				// Note you can PROBABLY access state_data here because we just checked state, but really this is undefined
				memcpy(dst, s, state_data.aline_size * sizeof(fftwf_complex));
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


