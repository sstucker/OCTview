import os
import sys
from fbs_runtime.application_context.PyQt5 import ApplicationContext
from controller import NIOCTController
from widgets import MainWindow
import sys
import qdarkstyle
import types


class _AppContext(ApplicationContext):

    def __init__(self):
        super().__init__()
        self.version = self.build_settings['version']
        self.name = self.build_settings['app_name']

        self._controller = None  # Controller is a member of AppContext but is managed by MainWindow

        # Dark theme
        self.app.setStyleSheet(qdarkstyle.load_stylesheet())

        self.ui_resource_location = str(self.get_resource('ui'))
        self.config_resource_location = str(self.get_resource('configurations'))

    def run(self):
        window = MainWindow()
        window.setWindowTitle(self.name + ' v' + self.version)
        window.resize(250, 150)
        window.show()
        # self._controller = NIOCTController()

        return self.app.exec_()


# Module interface
AppContext = _AppContext()
ui_resource_location = AppContext.ui_resource_location
config_resource_location = AppContext.config_resource_location
run = AppContext.run
