from fbs_runtime.application_context.PyQt5 import ApplicationContext
from controller import NIOCTController
from widgets import MainWindow
import sys
import qdarkstyle


class _AppContext(ApplicationContext):

    def __init__(self):
        super().__init__()
        self.version = self.build_settings['version']
        self.name = self.build_settings['app_name']

        self._controller = None  # Assigned by 'load'

        self.ui_resource_location = str(self.get_resource('ui'))
        self.config_resource_location = str(self.get_resource('configurations'))
        self.lib_resource_location = str(self.get_resource('lib'))

        self.window = None

    def load(self):
        if self.window is None:
            sys.exit('Failed to load GUI.')
        """Load backend"""
        if self.window.darkTheme:
            self.app.setStyleSheet(qdarkstyle.load_stylesheet())
        else:
            self.app.setStyleSheet('')
        # self._controller = NIOCTController()

    def run(self):
        self.window = MainWindow()
        self.window.setWindowTitle(self.name + ' v' + self.version)
        self.window.resize(250, 150)
        self.window.reload_required.connect(self.load)
        self.load()
        self.window.show()

        return self.app.exec_()

    # @property
    # def window(self):
    #     return self.window


# Module interface
AppContext = _AppContext()
ui_resource_location = AppContext.ui_resource_location
config_resource_location = AppContext.config_resource_location
lib_resource_location = AppContext.lib_resource_location
run = AppContext.run
