import ctypes
import os
import datetime
import json
import numpy as np
import qdarkstyle
from PyQt5.QtCore import QTimer, Qt
from PyQt5.QtGui import QFont
from fbs_runtime.application_context.PyQt5 import ApplicationContext

from controller import NIOCTController
from widgets import MainWindow

CALLBACK_DEBOUNCE_MS = 400
MAX_DISPLAY_UPDATE_RATE = 20
MAX_2D_IMAGE_SIZE = 128 * 128
MAX_ALINES_IN_SINGLE_BUFFER = 64 * 64

class _AppContext(ApplicationContext):

    def __init__(self):
        super().__init__()
        self.version = self.build_settings['version']
        self.name = self.build_settings['app_name']

        self.ui_resource_location = str(self.get_resource('ui'))
        self.config_resource_location = str(self.get_resource('configurations'))

        self.app.setAttribute(Qt.AA_EnableHighDpiScaling)

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

        # Assigned on run
        self.window = None  # Qt window
        self.controller = None  # Backend. This AppContext instance mediates their communication

        self._first_setup = True  # Set false at end of `run`

    def run(self):
        self.window = MainWindow()
        self.window.loadConfiguration(cfg_file=os.path.join(self.config_resource_location, '.last.ini'))
        self.window.launch.connect(self._launch)
        self.window.setWindowTitle(self.name + ' v' + self.version)
        self.app.setFont(QFont("Microsoft Sans Serif", 8))
        self._setup_window()
        # ... Do anything else before initial backend setup
        self._setup_backend()
        self._first_setup = False
        return self.app.exec_()

    def _launch(self):
        self._setup_window()
        self._setup_backend()

    def _setup_window(self):
        if self.window.darkTheme():
            self.app.setStyleSheet(qdarkstyle.load_stylesheet())
        else:
            self.app.setStyleSheet('')
        self.window.show()

    def _setup_backend(self):
        """Load or reload the backend and connect it to the GUI"""
        self._update_timer.stop()
        if not self._first_setup:
            self._close_controller()
        # Open controller
        if self._open_controller():  # If opening the backend succeeds

            # Connect MainWindow interaction signals to backend interface
            self.window.scan.connect(self._start_scanning)
            self.window.acquire.connect(self._start_acquisition)
            self.window.stop.connect(self._stop)
            self.window.closed.connect(self._close_controller)
            self.window.scan_changed.connect(self._configure_image)
            self.window.processing_changed.connect(self._configure_processing)

            self._configure_image()
            self._configure_processing(self.window.unprocessed_frame_size(), self.window.processed_frame_size())
            self._update_timer.start(100)  # 10 Hz
        else:
            self.window.print('OCTview: Failed to open controller. Try reconfiguring the application.')

    def _update(self):
        if self.controller is not None:
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
                self._image_buffer = np.reshape(self._grab_buffer, self.window.image_dimensions(), order='F')
                self.window.display_frame(self._image_buffer)
        if self._spectrum_buffer is not None:
            if self.controller.grab_spectrum(self._spectrum_buffer) > -1:
                self.window.display_spectrum(self._spectrum_buffer)
        # else:
        #     print("Failed to grab frame. Maybe one wasnt available yet")

    def _create_metadata_json(self):
        pat = self.window.scan_pattern()
        if self.window.frames_to_acquire() > -1:
            acq_n = str(self.window.frames_to_acquire())
        else:
            acq_n = 'Indefinite'
        if self.window.save_processed_data():
            processed = 'True'
            datatype = 'COMPLEX64'
            frame_size = self.window.processed_frame_size()
            zroi_start = self.window.roi_offset()
            zroi_size = self.window.roi_size()
            processing_params = {
                'Background-subtraction': str(self.window.background_subtraction_enabled()),
                'Wavenumber-linearization': str(self.window.interpolation_enabled()),
                'Apodization': str(np.max(self.window.apodization_window()) > 1),
                'A-line-repeat-processing': str(self.window.aline_repeat_processing()),
                'B-line-repeat-processing': str(self.window.bline_repeat_processing()),
                'FFT': 'True'
            }
        else:
            processed = 'False'
            datatype = 'UINT16'
            frame_size = self.window.unprocessed_frame_size()
            zroi_start = 0
            zroi_size = self.window.aline_size()
            processing_params = {}
        metadata = {
            'Created': datetime.datetime.now().strftime("%d/%m/%Y %H:%M:%S"),
            'OCTview version': self.version,
            'Data-type': datatype,
            'Processed': processed,
            'Processing-params': processing_params,
            'Number-of-frames': acq_n,
            'Frame-size-voxels': frame_size,
            'Spectrometer-bins': self.window.aline_size(),
            'Spectrometer-bandwidth-nm': self.window.spectrometer_range(),
            'Axial-voxel-size-mm': self.window.axial_pixel_size_mm(),
            'Axial-crop-offset': zroi_start,
            'Axial-crop-size': zroi_size,
            'A-lines': pat.dimensions[0],
            'A-line-repeat': pat.aline_repeat,
            'B-lines': pat.dimensions[1],
            'B-line-repeat': pat.bline_repeat,
            'Scan-pattern-type': str(pat.__class__.__name__),
            'XY-ROI-mm': tuple(pat.fov),
            'XY-Shift-mm': (self.window.scan_offset_mm_x(), self.window.scan_offset_mm_x()),
            'V/mm-scan-factors': self.window.scan_scale_factors(),
            'Pattern-rate-hz': pat.pattern_rate,
            'Line-camera-trigger-rate-hz': self.window.max_line_rate()
        }
        with open(self.window.filename() + '.json', 'w') as f:
            json.dump(metadata, f)
    # -- Backend interface ------------------------------------------------

    def _open_controller(self):
        # Could switch between various backends here if you wanted
        for path in self.window.dll_search_paths():
            if os.path.exists(path):
                try:
                    if f.endswith('.dll'):
                        print('OCTview: Loading', os.path.join(path, f))
                        ctypes.cdll.LoadLibrary(os.path.join(path, f))
                except (OSError, FileNotFoundError):
                    print('OCTview: Failed to load library', os.path.join(path, f))
                for f in os.listdir(path):
                    pass
        self.controller = NIOCTController(os.path.join(self.get_resource('fastnisdoct.dll')))
        self.controller.open(
            self.window.camera_device_name(),
            self.window.analog_output_galvo_x_ch_name(),
            self.window.analog_output_galvo_y_ch_name(),
            self.window.analog_output_line_trig_ch_name(),
            self.window.analog_output_start_trig_ch_name()
        )
        return True

    def _close_controller(self):
        if self.controller is not None:
            self.controller.close()

    def _configure_image(self):
        self._ctr_configure_image += 1
        self._timer_configure_image.singleShot(CALLBACK_DEBOUNCE_MS, self._configure_image_cb)

    def _configure_image_cb(self):
        self._ctr_configure_image -= 1
        if not self._ctr_configure_image > 0:
            pat = self.window.scan_pattern()
            scan_x = (pat.x + self.window.scan_offset_mm_x()) * self.window.scan_scale_factors()[0]
            scan_y = (pat.y + self.window.scan_offset_mm_y()) * self.window.scan_scale_factors()[1]
            scan_line_trig = pat.line_trigger * self.window.trigger_gain()
            all_samples = np.concatenate([scan_y, scan_x])
            self.controller.configure_image(
                aline_size=self.window.aline_size(),
                alines_in_scan=pat.points_in_scan,
                image_mask=pat.image_mask,
                alines_in_image=pat.points_in_image,
                alines_per_bline=pat.dimensions[0] * pat.aline_repeat * pat.bline_repeat,
                alines_per_buffer=self.window.alines_per_buffer(),
                frames_to_buffer=self.window.frames_to_buffer(),
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
            self._configure_image()
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
            print('OCTview: Polling display buffer every', t, 'ms,', 1 / (t / 1000), 'Hz')
            self._display_update_timer.start(t)

    def _start_acquisition(self):
        if self.controller.state == 'ready' or self.controller.state == 'scanning':
            if self.controller.state == 'ready':
                self.controller.start_scan()
                self._display_update_timer.start(int(1 / self.window.scan_pattern().pattern_rate * 1000))
            if self.window.should_create_metadata_file():
                self._create_metadata_json()
            self.controller.start_acquisition(
                self.window.filename(),
                self.window.file_max_gb(),
                self.window.frames_to_acquire(),
                self.window.save_processed_data()
            )

    def _stop(self):
        self.controller.stop_scan()
        self._display_update_timer.stop()


# Module interface
AppContext = _AppContext()
__version__ = AppContext.version
version = AppContext.version
print("===============================")
print("OCTview v" + version)
print("===============================")
window = AppContext.window
ui_resource_location = AppContext.ui_resource_location
config_resource_location = AppContext.config_resource_location
run = AppContext.run
