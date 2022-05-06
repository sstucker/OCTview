import ctypes
import os
import sys
import time
import numpy as np
import qdarkstyle
from PyQt5.QtWidgets import QSplashScreen
from PyQt5.QtCore import QTimer, Qt
from PyQt5.QtGui import QFont
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

    def run(self):
        # Debounce timers
        self._timer_configure_image = QTimer()
        self._ctr_configure_image = 0

        self._timer_configure_processing = QTimer()
        self._ctr_configure_processing = 0

        self._timer_update_scan_pattern = QTimer()
        self._ctr_update_scan_pattern = 0

        # Record these sizes to decide how to call backend
        self._uncropped_frame_size = 0
        self._unprocessed_frame_size = 0
        self._processed_frame_size = 0
        self._bline_proc_mode = 0

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

        self.window.loadConfiguration(cfg_file=os.path.join(self.config_resource_location, '.last'))

        # ... Do anything else before backend setup

        self._launch()

        return self.app.exec_()

    def _launch(self):
        # Configure/reconfigure GUI, load backend

        if self.window is None:
            sys.exit('Failed to load GUI.')

        if self.window.darkTheme:
            self.app.setStyleSheet(qdarkstyle.load_stylesheet())
        else:
            self.app.setStyleSheet('')
        self.app.setFont(QFont("Microsoft Sans Serif", 8))

        # Connect MainWindow interaction signals to backend interface
        self.window.scan.connect(self._start_scanning)
        self.window.acquire.connect(self._start_acquisition)
        self.window.stop.connect(self._stop)
        self.window.closed.connect(self._close_controller)
        self.window.scan_changed.connect(self._configure_image)
        self.window.processing_changed.connect(self._configure_processing)
        self.window.launch.connect(self._launch)

        # Open controller
        self._open_controller()
        self._configure_image()
        self._configure_processing(self.window.unprocessed_frame_size(), self.window.processed_frame_size())
        self._update_timer.start(100)  # 10 Hz

        self.window.show()

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
        if self._grab_buffer is not None and self._image_buffer is not None:
            if self.controller.grab_frame(self._grab_buffer) > -1:
                # print('Copying to display buffer with shape', np.shape(self._image_buffer))
                i = 0
                for x in range(self.window.scan_pattern().dimensions[0]):
                    for y in range(self.window.scan_pattern().dimensions[1]):
                        self._image_buffer[:, x, y] = self._grab_buffer[
                                                      self.window.roi_size() * i:self.window.roi_size() * i + self.window.roi_size()
                                                      ]
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
            self.window.number_of_image_buffers()
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
            scan_x = pat.x * self.window.scan_scale_factors()[0]
            scan_y = pat.y * self.window.scan_scale_factors()[1]
            scan_line_trig = pat.line_trigger * self.window.trigger_gain()
            import matplotlib.pyplot as plt
            # plt.subplot(2, 1, 1)
            # plt.plot(scan_x)
            # plt.plot(scan_y)
            # plt.plot(scan_line_trig)
            # plt.subplot(2, 1, 2)
            # plt.stem(pat.image_mask)
            # plt.show()
            all_samples = np.concatenate([scan_y, scan_x])
            print("Updating pattern generation signals. Range:", np.min(all_samples), np.max(all_samples), 'Rate:',
                  self.window.scan_pattern().sample_rate, 'Length:', len(scan_x))
            self.controller.configure_image(
                aline_size=self.window.aline_size(),
                alines_in_scan=pat.points_in_scan,
                image_mask=pat.image_mask,
                alines_in_image=pat.points_in_image,
                alines_per_bline=pat.dimensions[0] * pat.aline_repeat * pat.bline_repeat,
                alines_per_buffer=self.window.alines_per_buffer(),
                x_scan_signal=scan_x,
                y_scan_signal=scan_y,
                line_trigger_scan_signal=scan_line_trig,
                signal_output_rate=pat.sample_rate,
                line_rate=pat.max_trigger_rate,
                aline_repeat=pat.aline_repeat,
                bline_repeat=pat.bline_repeat,
                roi_offset=self.window.roi_offset(),
                roi_size=self.window.roi_size(),
                aline_repeat_processing=self.window.aline_repeat_processing(),
                bline_repeat_processing=self.window.bline_repeat_processing()
            )
            self._uncropped_frame_size = self.window.uncropped_frame_size()
            self._unprocessed_frame_size = self.window.unprocessed_frame_size()
            self._processed_frame_size = self.window.processed_frame_size()
            processed_shape = self.window.image_dimensions()
            self._image_buffer = np.zeros(processed_shape, dtype=np.complex64)
            self._grab_buffer = np.zeros(self._processed_frame_size, dtype=np.complex64)
            self._spectrum_buffer = np.zeros(self.window.aline_size(), dtype=np.float32)
            self._bline_proc_mode = self.window.bline_repeat_processing()

    def _configure_processing(self, unprocessed_frame_size: int, processed_frame_size: int):
        # Because some changes to processing are really changes to the IMAGE at the controller level, do this check
        if self._unprocessed_frame_size != unprocessed_frame_size or self._processed_frame_size != processed_frame_size or self._bline_proc_mode != self.window.bline_repeat_processing():
            print('Frame sizes changed! {} -> {}, {} -> {}'.format(self._unprocessed_frame_size, unprocessed_frame_size,
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
                self.window.background_subtraction_enabled(),
                self.window.interpolation_enabled(),
                self.window.interpdk(),
                self.window.apodization_window(),
                n_frame_avg=self.window.frame_averaging()
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
            self.window.file_max_gb(),
            self.window.frames_to_acquire()
        )

    def _stop(self):
        self.controller.stop_acquisition()
        self.controller.stop_scan()
        self._display_update_timer.stop()


# Module interface
AppContext = _AppContext()
window = AppContext.window
ui_resource_location = AppContext.ui_resource_location
config_resource_location = AppContext.config_resource_location
lib_resource_location = AppContext.lib_resource_location
run = AppContext.run
