from fbs_runtime.application_context.PyQt5 import ApplicationContext
from PyQt5.QtWidgets import QMainWindow
from PyQt5.QtCore import QFile, QTextStream

from controller import NIOCTController
from widgets import MainWindow

import sys
import qdarkstyle


class AppContext(ApplicationContext):

    def __init__(self):
        super().__init__()
        self._version = self.build_settings['version']
        self._name = self.build_settings['app_name']

        self._controller = None  # Controller is a member of AppContext but is managed by MainWindow

        # Dark theme
        self.app.setStyleSheet(qdarkstyle.load_stylesheet())

    def run(self):
        window = MainWindow()
        window.setWindowTitle(self._name + ' v' + self._version)
        window.resize(250, 150)
        window.show()
        # self._controller = NIOCTController()

        return self.app.exec_()


if __name__ == '__main__':
    appctxt = AppContext()
    exit_code = appctxt.run()
    sys.exit(exit_code)