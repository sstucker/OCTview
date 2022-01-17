from PyQt5.QtWidgets import QWidget, QGridLayout, QGroupBox, QMainWindow, QSpinBox, QDoubleSpinBox, QCheckBox, QRadioButton

import pyqtgraph as pyqtg
import os
from PyQt5 import QtCore
from PyQt5 import uic
from PyQt5.QtCore import pyqtSignal, QObject


class UiWidget:

    def __init__(self, uiname: str = None):
        """Base class for QWidget or QObject that uses a Qt Creator ui file.

        Automatically loads the .ui file with the same name as the class. Provides convenience function 'replaceWidget'
        for dynamically replacing placeholder Widgets in the .ui file.

        Args:
            uiname (str): Optional kwarg. The .ui file to load from. Must be located in src/main/ui folder relative to project
                directory. If not provided, .ui file is assumed to be same as the class.
        """
        super().__init__()

        # by default, load .ui file of the same name as the class
        if uiname is None:
            uiname = self.__class__.__name__
        ui = os.sep.join([os.getcwd(), 'src', 'main', 'ui', uiname + '.ui'])
        uic.loadUi(ui, self)

    def replaceWidget(self, old_widget: QWidget, new_widget: QWidget):
        """Replace a widget with another."""
        plotwidget_placeholder_layout = old_widget.parent().layout()
        plotwidget_placeholder_layout.replaceWidget(old_widget, new_widget)
        new_widget.setObjectName(old_widget.objectName())
        old_widget.hide()
        new_widget.show()
        self.__dict__[old_widget.objectName()] = new_widget  # Goodbye, old widget reference!


class DisplayWidget(QWidget, UiWidget):

    def __init__(self):
        super(QWidget, self).__init__()
        super(UiWidget, self).__init__()


class RasterScanWidget(QWidget, UiWidget):

    def __init__(self):
        super(QWidget, self).__init__()
        super(UiWidget, self).__init__()


class LineScanWidget(QWidget, UiWidget):

    def __init__(self):
        super(QWidget, self).__init__()
        super(UiWidget, self).__init__()


class CircleScanWidget(QWidget, UiWidget):

    def __init__(self):
        super(QWidget, self).__init__()
        super(UiWidget, self).__init__()


class ControlGroupBox(QGroupBox, UiWidget):

    scan = pyqtSignal()
    acquire = pyqtSignal()
    snap = pyqtSignal()
    stop = pyqtSignal()

    def __init__(self):
        super(QGroupBox, self).__init__()
        super(UiWidget, self).__init__()

        self.pushScan.pressed.connect(self.scan.emit)
        self.pushStop.pressed.connect(self.stop.emit)
        self.pushAcquire.pressed.connect(self.acquire.emit)
        self.pushSnap.pressed.connect(self.stop.emit)


class ScanGroupBox(QGroupBox, UiWidget):

    update = pyqtSignal()

    def __init__(self):
        super(QGroupBox, self).__init__()
        super(UiWidget, self).__init__()

        self._subwidgets = {
            'Raster': RasterScanWidget(),
            'Line': LineScanWidget(),
            'Circle': CircleScanWidget()
        }
        self._selectSubwidget()
        self.comboScanPattern.currentIndexChanged.connect(self._selectSubwidget)

        # All widgets should emit the scan pattern update signal
        children = self.children()
        for subwidget in list(self._subwidgets.values()):
            children += subwidget.children()
        for child in children:
            if type(child) in [QSpinBox, QDoubleSpinBox, QRadioButton]:
                child.valueChanged.connect(self.update.emit)
            elif type(child) in [QCheckBox]:
                child.stateChanged.connect(self.update.emit)

        self.update.connect(lambda: print('Scan pattern updated!'))  # TODO :debug: remove

    def _selectSubwidget(self):
        self.replaceWidget(self.ScanWidget, self._subwidgets[self.comboScanPattern.currentText()])



class FileGroupBox(QGroupBox, UiWidget):

    def __init__(self):
        super(QGroupBox, self).__init__()
        super(UiWidget, self).__init__()


class ProcessingGroupBox(QGroupBox, UiWidget):

    def __init__(self):
        super(QGroupBox, self).__init__()
        super(UiWidget, self).__init__()


class DisplayWidget(QWidget, UiWidget):

    def __init__(self):
        super(QWidget, self).__init__()
        super(UiWidget, self).__init__()


class SpectrumWidget(QWidget, UiWidget):

    def __init__(self):
        super(QWidget, self).__init__()
        super(UiWidget, self).__init__()


class MainWindow(QMainWindow, UiWidget):

    def __init__(self):
        super(QMainWindow, self).__init__()
        super(UiWidget, self).__init__()

        self.replaceWidget(self.ScanGroupBox, ScanGroupBox())
        self.replaceWidget(self.DisplayWidget, DisplayWidget())
        self.replaceWidget(self.FileGroupBox, FileGroupBox())
        self.replaceWidget(self.SpectrumWidget, SpectrumWidget())
        self.replaceWidget(self.ProcessingGroupBox, ProcessingGroupBox())
        self.replaceWidget(self.ControlGroupBox, ControlGroupBox())

        self.show()

        self.statusBar().setSizeGripEnabled(False)
        self.setFixedSize(self.minimumSize())

