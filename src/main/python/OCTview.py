import ctypes
import os
import sys

import numpy as np
import qdarkstyle
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

    def load(self):
        if self.window is None:
            sys.exit('Failed to load GUI.')
        if self.window.darkTheme:
            self.app.setStyleSheet(qdarkstyle.load_stylesheet())
        else:
            self.app.setStyleSheet('')

    def run(self):
        self.window = MainWindow()
        self.window.setWindowTitle(self.name + ' v' + self.version)
        self.window.resize(250, 150)

        # Connect MainWindow signals to backend interface
        self.window.reload_required.connect(self.load)
        self.window.scan.connect(self._start_scanning)
        self.window.acquire.connect(self._start_acquisition)
        self.window.stop.connect(self._stop)
        self.window.closed.connect(self._close_controller)
        self.window.scan_changed.connect(self._update_scan_pattern)
        self.window.processing_changed.connect(self._configure_processing)

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
        self._update_timer.start(100)  # 10 Hz

        # Periodic display of grabbed frame
        self._display_update_timer = QTimer()
        self._display_update_timer.timeout.connect(self._display_update)
        self._display_buffer = None

        self.window.show()
        self._open_controller()

        self.load()
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
        if self.controller.grab_frame(self._grab_buffer) > -1:
            print('Copying to display buffer with shape', np.shape(self._display_buffer))
            i = 0
            for x in range(self.window.scan_pattern().dimensions[0]):
                for y in range(self.window.scan_pattern().dimensions[1]):
                    self._display_buffer[:, x, y] = self._grab_buffer[self.window.roi_size() * i:self.window.roi_size() * i + self.window.roi_size()]
                    i += 1
            self.window.display_frame(self._display_buffer)
            print("Grabbed a frame size", self._processed_frame_size, 'max', np.max(self._display_buffer))
        else:
            print("Failed to grab frame. Maybe one wasnt available yet")

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
        (zstart, zstop) = self.window.zroi()
        self._ctr_configure_image -= 1
        if not self._ctr_configure_image > 0:
            self.controller.configure_image(
                self.window.max_line_rate(),
                self.window.aline_size(),
                self.window.scan_pattern().total_number_of_alines,
                self.window.scan_pattern().dimensions[0],
                self.window.scan_pattern().aline_repeat,
                self.window.scan_pattern().bline_repeat,
                self.window.number_of_image_buffers(),
                self.window.roi_offset(),
                self.window.roi_size()
            )
            self._processed_frame_size = self.window.processed_frame_size()
            self._raw_frame_size = self.window.raw_frame_size()
            self._grab_buffer = np.zeros(self._processed_frame_size, dtype=np.complex64)
            processed_shape = (self.window.roi_size(), self.window.scan_pattern().dimensions[0], self.window.scan_pattern().dimensions[1])
            self._display_buffer = np.zeros(processed_shape, dtype=np.complex64)
            print('...Frame sizes updated:', self._raw_frame_size, self._processed_frame_size)

    def _configure_processing(self):
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
                aline_repeat_processing=self.window.aline_repeat_processing(),
                bline_repeat_processing=self.window.bline_repeat_processing(),
                n_frame_avg=self.window.frame_averaging()
            )

    def _update_scan_pattern(self, raw_frame_size: int, processed_frame_size: int):
        # If frame sizes are changing with this pattern update, reconfigure
        if self._raw_frame_size != raw_frame_size or self._processed_frame_size != processed_frame_size:
            print('Frame sizes changed! {} -> {}, {} -> {}'.format(self._raw_frame_size, raw_frame_size,
                                                                   self._processed_frame_size, processed_frame_size))
            self._configure_image()
            self._configure_processing()
        else:
            print('Updating scan pattern with pattern of the same size.')
        self._ctr_update_scan_pattern += 1
        self._timer_update_scan_pattern.singleShot(CALLBACK_DEBOUNCE_MS, self._update_scan_pattern_cb)

    def _update_scan_pattern_cb(self):
        self._ctr_update_scan_pattern -= 1
        if not self._ctr_update_scan_pattern > 0:
            all_samples = np.concatenate([self.window.scan_pattern().x * self.window.scan_scale_factors()[0], self.window.scan_pattern().y * self.window.scan_scale_factors()[1]])
            print("Updating pattern generation signals. Range:", np.min(all_samples), np.max(all_samples))
            self.controller.set_scan(
                self.window.scan_pattern().x * self.window.scan_scale_factors()[0],
                self.window.scan_pattern().y * self.window.scan_scale_factors()[1],
                self.window.scan_pattern().line_trigger * self.window.trigger_gain(),
                self.window.scan_pattern().frame_trigger * self.window.trigger_gain(),
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
