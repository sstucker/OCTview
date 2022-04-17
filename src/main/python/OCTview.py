import ctypes
import os
import sys

import numpy as np
import qdarkstyle
from PyQt5.QtWidgets import QSplashScreen
from PyQt5.QtCore import QTimer, Qt
from fbs_runtime.application_context.PyQt5 import ApplicationContext

from controller import NIOCTController
from widgets import MainWindow

CALLBACK_DEBOUNCE_MS = 400
MAX_DISPLAY_UPDATE_RATE = 20


class _AppContext(ApplicationContext):

    def __init__(self):
        super().__init__()
        self.version = self.build_settings['version']
        self.name = self.build_settings['app_name']

        self.ui_resource_location = str(self.get_resource('ui'))
        self.config_resource_location = str(self.get_resource('configurations'))
        self.lib_resource_location = str(self.get_resource('lib'))

        self.app.setAttribute(Qt.AA_EnableHighDpiScaling)

        # Assigned on run
        self.window = None  # Qt window
        self.controller = None  # Backend. This AppContext instance mediates their communication

    def _launch(self):
        # Configure/reconfigure GUI, load backend

        if self.window is None:
            sys.exit('Failed to load GUI.')

        if self.window.darkTheme:
            self.app.setStyleSheet(qdarkstyle.load_stylesheet())
        else:
            self.app.setStyleSheet('')

        # Connect MainWindow interaction signals to backend interface
        self.window.scan.connect(self._start_scanning)
        self.window.acquire.connect(self._start_acquisition)
        self.window.stop.connect(self._stop)
        self.window.closed.connect(self._close_controller)
        self.window.scan_changed.connect(self._update_scan_pattern)
        self.window.processing_changed.connect(self._configure_processing)
        self.window.launch.connect(self._launch)

        # Open controller
        self._open_controller()
        self._update_timer.start(100)  # 10 Hz

        self.window.show()

    def run(self):

        # Debounce timers
        self._timer_configure_image = QTimer()
        self._ctr_configure_image = 0

        self._timer_configure_processing = QTimer()
        self._ctr_configure_processing = 0

        self._timer_update_scan_pattern = QTimer()
        self._ctr_update_scan_pattern = 0

        # Record these sizes to optimize calls to interface
        self._raw_frame_size = 0
        self._processed_frame_size = 0

        # Repeated update execution keeps GUI in step with controller
        self._update_timer = QTimer()
        self._update_timer.timeout.connect(self._update)

        # Periodic display of grabbed frame
        self._display_update_timer = QTimer()
        self._display_update_timer.timeout.connect(self._display_update)
        self._image_buffer = None
        self._spectrum_buffer = None

        self.window = MainWindow()
        self.window.setWindowTitle(self.name + ' v' + self.version)
        self.window.resize(250, 150)

        # Load the window's state from .last JSON
        # Will emit all the signals but we have not connected them yet
        self.window.loadConfiguration(file=os.path.join(self.config_resource_location, '.last'))

        # ... Do anything else before backend setup

        self._launch()

        return self.app.exec_()

    def _update(self):
        state = self.controller.state
        if state == 'scanning':
            # print('GUI in scanning state')
            self.window.set_mode_scanning()
        elif state == 'acquiring':
            # print('GUI in acquiring state')
            self.window.set_mode_acquiring()
        elif state == 'ready':
            # print('GUI in ready state')
            self.window.set_mode_ready()
        elif state == 'open' or state == 'error' or state == 'unopened':
            # print('GUI in unready state:', state)
            self.window.set_mode_not_ready()

    def _display_update(self):
        print(self.window.image_dimensions())
        if self._grab_buffer is not None and self._image_buffer is not None:
            if self.controller.grab_frame(self._grab_buffer) > -1:
                # print('Copying to display buffer with shape', np.shape(self._image_buffer))
                i = 0
                for x in range(self.window.scan_pattern().dimensions[0]):
                    for y in range(self.window.scan_pattern().dimensions[1]):
                        self._image_buffer[:, x, y] = self._grab_buffer[
                                                        self.window.roi_size() * i:self.window.roi_size() * i + self.window.roi_size()]
                        i += 1
                self.window.display_frame(self._image_buffer)
        if self._spectrum_buffer is not None:
            if self.controller.grab_spectrum(self._spectrum_buffer) > -1:
                self.window.display_spectrum(self._spectrum_buffer)
        # else:
        #     print("Failed to grab frame. Maybe one wasnt available yet")

    # -- Backend interface ------------------------------------------------

    def _open_controller(self):
        # Could switch between various backends here if you wanted
        ctypes.cdll.LoadLibrary(os.path.join(self.lib_resource_location, 'fastnisdoct/libfftw3f-3.dll'))
        self.controller = NIOCTController(os.path.join(self.lib_resource_location, 'fastnisdoct/fastnisdoct.dll'))
        self.controller.open(
            self.window.camera_device_name(),
            self.window.analog_output_galvo_x_ch_name(),
            self.window.analog_output_galvo_y_ch_name(),
            self.window.analog_output_line_trig_ch_name(),
            self.window.analog_output_frame_trig_ch_name(),
            self.window.analog_output_start_trig_ch_name(),
        )

    def _close_controller(self):
        self.controller.close()

    def _configure_image(self):
        self._ctr_configure_image += 1
        self._timer_configure_image.singleShot(CALLBACK_DEBOUNCE_MS, self._configure_image_cb)

    def _configure_image_cb(self):
        self._ctr_configure_image -= 1
        if not self._ctr_configure_image > 0:
            pat = self.window.scan_pattern()
            self.controller.configure_image(
                aline_size=self.window.aline_size(),
                alines_in_scan=pat.alines_in_scan,
                image_mask=pat.image_mask,
                alines_in_image=pat.total_number_of_alines,
                alines_per_b=pat.dimensions[0],
                aline_repeat=pat.aline_repeat,
                bline_repeat=pat.bline_repeat,
                number_of_buffers=self.window.number_of_image_buffers(),
                roi_offset=self.window.roi_offset(),
                roi_size=self.window.roi_size(),
                buffer_blines=pat.trigger_blines,
                aline_repeat_processing=self.window.aline_repeat_processing(),
                bline_repeat_processing=self.window.bline_repeat_processing(),

            )
            self._processed_frame_size = self.window.processed_frame_size()
            self._raw_frame_size = self.window.raw_frame_size()
            processed_shape = self.window.image_dimensions()
            self._image_buffer = np.zeros(processed_shape, dtype=np.complex64)
            self._grab_buffer = np.zeros(self._processed_frame_size, dtype=np.complex64)
            self._spectrum_buffer = np.zeros(self.window.aline_size(), dtype=np.float32)

    def _configure_processing(self, raw_frame_size: int, processed_frame_size: int):
        if self._raw_frame_size != raw_frame_size or self._processed_frame_size != processed_frame_size:
            print('Frame sizes changed! {} -> {}, {} -> {}'.format(self._raw_frame_size, raw_frame_size,
                                                                   self._processed_frame_size, processed_frame_size))
            self._configure_image()
        else:
            print('Updating processing with image of the same size.')
        self._ctr_configure_processing += 1
        self._timer_configure_processing.singleShot(CALLBACK_DEBOUNCE_MS, self._configure_processing_cb)

    def _configure_processing_cb(self):
        self._ctr_configure_processing -= 1
        if not self._ctr_configure_processing > 0:
            self.controller.configure_processing(
                self.window.processing_enabled(),
                self.window.background_subtraction_enabled(),
                self.window.interpolation_enabled(),
                self.window.interpdk(),
                self.window.apodization_window(),
                n_frame_avg=self.window.frame_averaging()
            )

    def _update_scan_pattern(self, raw_frame_size: int, processed_frame_size: int):
        # If frame sizes are changing with this pattern update, reconfigure
        if self._raw_frame_size != raw_frame_size or self._processed_frame_size != processed_frame_size:
            print('Frame sizes changed! {} -> {}, {} -> {}'.format(self._raw_frame_size, raw_frame_size,
                                                                   self._processed_frame_size, processed_frame_size))
            self._configure_image()
            self._configure_processing(raw_frame_size, processed_frame_size)
        else:
            print('Updating scan pattern with pattern of the same size.')
        self._ctr_update_scan_pattern += 1
        self._timer_update_scan_pattern.singleShot(CALLBACK_DEBOUNCE_MS, self._update_scan_pattern_cb)

    def _update_scan_pattern_cb(self):
        self._ctr_update_scan_pattern -= 1
        if not self._ctr_update_scan_pattern > 0:
            scan_x = self.window.scan_pattern().x * self.window.scan_scale_factors()[0]
            scan_y = self.window.scan_pattern().y * self.window.scan_scale_factors()[1]

            scan_line_trig = self.window.scan_pattern().line_trigger * self.window.trigger_gain()

            scan_frame_trig = self.window.scan_pattern().frame_trigger * self.window.trigger_gain()
            # scan_frame_trig = np.zeros(len(scan_frame_trig)).astype(np.float64)
            #
            # t_frame_trig_start = np.where(scan_line_trig > 0)[0][0]
            # t_frame_trig_end = np.where(scan_line_trig > 0)[0][-1]

            # scan_frame_trig[t_frame_trig_start:t_frame_trig_end] = self.window.trigger_gain()
            # scan_frame_trig[t_frame_trig_start-1:t_frame_trig_start+4] = self.window.trigger_gain()
            #
            scan_line_trig = np.zeros(len(scan_line_trig)).astype(np.float64)
            # scan_line_trig[0::2] = self.window.trigger_gain()

            # import matplotlib.pyplot as plt
            # plt.plot(scan_line_trig)
            # plt.plot(scan_frame_trig)
            # plt.plot(scan_x)
            # plt.show()

            all_samples = np.concatenate([scan_y, scan_x])
            print("Updating pattern generation signals. Range:", np.min(all_samples), np.max(all_samples), 'Rate:',
                  self.window.scan_pattern().sample_rate, 'Length:', len(scan_x))
            self.controller.set_scan(
                scan_x,
                scan_y,
                scan_line_trig,
                scan_frame_trig,
                self.window.scan_pattern().sample_rate,
            )

    def _start_scanning(self):
        if self.controller.state == 'acquiring':
            self.controller.stop_acquisition()
        else:
            self.controller.start_scan()
            t = max(int(1 / self.window.scan_pattern().pattern_rate * 1000), int(1 / MAX_DISPLAY_UPDATE_RATE * 1000))
            print('Polling display buffer every', t, 'ms,', 1 / (t / 1000), 'Hz')
            self._display_update_timer.start(t)

    def _start_acquisition(self):
        if self.controller.state != 'scanning' and self.controller.state == 'ready':
            self.controller.start_scan()
            self._display_update_timer.start(int(1 / self.window.scan_pattern().pattern_rate * 1000))
        self.controller.start_acquisition(
            self.window.filename(),
            self.window.file_max_bytes(),
            self.window.frames_to_acquire()
        )

    def _stop(self):
        self.controller.stop_acquisition()
        self.controller.stop_scan()
        self._display_update_timer.stop()


# Module interface
AppContext = _AppContext()
ui_resource_location = AppContext.ui_resource_location
config_resource_location = AppContext.config_resource_location
lib_resource_location = AppContext.lib_resource_location
run = AppContext.run
