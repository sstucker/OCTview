from fbs_runtime.application_context.PyQt5 import ApplicationContext
from PyQt5.QtCore import QTimer
from controller import NIOCTController
from widgets import MainWindow
import sys
import qdarkstyle
from controller import NIOCTController
import os


class _AppContext(ApplicationContext):

    def __init__(self):
        super().__init__()
        self.version = self.build_settings['version']
        self.name = self.build_settings['app_name']

        self.ui_resource_location = str(self.get_resource('ui'))
        self.config_resource_location = str(self.get_resource('configurations'))
        self.lib_resource_location = str(self.get_resource('lib'))

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
        self.window.scan.connect(self._start_scan)
        # self.window.acquire.connect(self._start_acquisition)
        self.window.stop.connect(self._stop)
        self.window.closed.connect(self._close_controller)

        self.load()
        self._open_controller()
        self._configure_image()
        self._configure_processing()
        self._update_scan_pattern()
        self.window.show()

        return self.app.exec_()

    # -- Backend interface ------------------------------------------------

    def _start_scan(self):
        self.controller.start_scan()

    def _stop_scan(self):
        self.controller.start_scan()

    def _open_controller(self):
        # Could switch between various backends here if you wanted
        self.controller = NIOCTController(os.path.join(self.lib_resource_location, 'fastnisdoct/fastnisdoct.dll'))
        self.controller.open(
            self.window.camera_device_name,
            self.window.analog_output_galvo_x_ch_name,
            self.window.analog_output_galvo_y_ch_name,
            self.window.analog_output_line_trig_ch_name,
            self.window.analog_output_frame_trig_ch_name,
            self.window.analog_output_start_trig_ch_name,
        )

    def _close_controller(self):
        self.controller.close()

    def _configure_image(self):
        (zstart, zstop) = self.window.zroi
        self.controller.configure_image(
            self.window.max_line_rate,
            self.window.aline_size,
            self.window.scan_pattern.total_number_of_alines,
            self.window.scan_pattern.dimensions[0],
            self.window.scan_pattern.aline_repeat,
            self.window.scan_pattern.bline_repeat,
            self.window.number_of_image_buffers,
            zstart,
            zstop
        )

    def _configure_processing(self):
        self.controller.configure_processing(
            self.window.processing_enabled,
            self.window.background_subtraction_enabled,
            self.window.interpolation_enabled,
            self.window.interpdk,
            self.window.apodization_window,
            aline_repeat_processing=self.window.aline_repeat_processing,
            bline_repeat_processing=self.window.bline_repeat_processing,
            n_frame_avg=self.window.frame_averaging
        )

    def _update_scan_pattern(self):
        self.controller.set_scan(
            self.window.scan_pattern.x,
            self.window.scan_pattern.y,
            self.window.scan_pattern.line_trigger,
            self.window.scan_pattern.frame_trigger,
        )

    def _start_scanning(self):
        self.controller.start_scan()

    # def _start_acquisition(self):
    #     self.controller.start_acquisition()

    def _stop(self):
        self.controller.stop_acquisition()
        self.controller.stop_scan()


# Module interface
AppContext = _AppContext()
ui_resource_location = AppContext.ui_resource_location
config_resource_location = AppContext.config_resource_location
lib_resource_location = AppContext.lib_resource_location
run = AppContext.run
