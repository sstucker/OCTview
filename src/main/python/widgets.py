from PyQt5.QtWidgets import QWidget, QGridLayout, QGroupBox, QMainWindow, QSpinBox, QDoubleSpinBox, QCheckBox, QRadioButton

import pyqtgraph as pyqtg
import os
from PyQt5 import QtCore
from PyQt5 import uic
from PyQt5.QtCore import pyqtSignal, QObject

from scanpatterns import LineScanPattern, RasterScanPattern, CircleScanPattern
import numpy as np

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
        super().__init__()


class ScanWidget(QWidget):

    def __init__(self):
        super().__init__()
        self._pattern = LineScanPattern()

    def generate_pattern(self):
        raise NotImplementedError()

    @property
    def x(self):
        return self._pattern.get_x()

    @property
    def y(self):
        return self._pattern.get_x()

    @property
    def line_trigger(self):
        return self._pattern.line_trigger()

    @property
    def frame_trigger(self):
        return self._pattern.frame_trigger()


class RasterScanWidget(ScanWidget, UiWidget):

    def __init__(self):
        super().__init__()

        self.checkSquareScan.stateChanged.connect(self._aspectChanged)
        self.checkEqualAspect.stateChanged.connect(self._aspectChanged)
        self.checkBidirectional.stateChanged.connect(self._checkBidirectionalChanged)
        self.checkARepeat.stateChanged.connect(self._checkARepeatChanged)
        self.checkBRepeat.stateChanged.connect(self._checkBRepeatChanged)
        self.spinACount.valueChanged.connect(self._xValueChanged)
        self.spinROIWidth.valueChanged.connect(self._xValueChanged)

    def generate_pattern(self):
        self._pattern = RasterScanPattern()
        self._pattern.generate(
            alines = self.spinACount.value(),
            blines = self.spinBCount.value(),
            max_trigger_rate = 76000,
            fov = [self.spinROIWidth.value(), self.spinROIHeight.value()],
            flyback_duty = 0.2,
            exposure_fraction = 0.8,
            fast_axis_step = self.radioXStep.isChecked(),
            slow_axis_step = self.radioYStep.isChecked(),
            aline_repeat = [1, self.spinARepeat.value()][int(self.checkARepeat.isChecked())],
            bline_repeat = [1, self.spinBRepeat.value()][int(self.checkBRepeat.isChecked())],
            bidirectional = self.checkBidirectional.isChecked(),
            rotation_rad = self.parentWidget().spinRotation.value() * np.pi / 180,
        )
        self.parentWidget().linePatternRate.setText(str(self._pattern.pattern_rate)[0:5] + ' Hz')

    def _aspectChanged(self):
        if self.checkEqualAspect.isChecked():
            self.spinROIHeight.setValue(self.spinROIWidth.value())
            self.spinROIHeight.setEnabled(False)
        else:
            self.spinROIHeight.setEnabled(True)
        if self.checkSquareScan.isChecked():
            self.spinBCount.setValue(self.spinACount.value())
            self.spinBCount.setEnabled(False)
        else:
            self.spinBCount.setEnabled(True)
        self.generate_pattern()

    def _checkBidirectionalChanged(self):
        if self.checkBidirectional.isChecked():
            self.checkARepeat.setChecked(False)
            self.checkBRepeat.setChecked(False)
            self.radioXSaw.setChecked(True)
            self.radioYSaw.setChecked(True)
        self.generate_pattern()

    def _checkARepeatChanged(self):
        if self.checkARepeat.isChecked():
            self.checkBidirectional.setChecked(False)
            self.spinARepeat.setEnabled(True)
            self.radioXStep.setChecked(True)
        else:
            self.spinARepeat.setEnabled(False)
        self.generate_pattern()

    def _checkBRepeatChanged(self):
        if self.checkBRepeat.isChecked():
            self.checkBidirectional.setChecked(False)
            self.spinBRepeat.setEnabled(True)
        else:
            self.spinBRepeat.setEnabled(False)
        self.generate_pattern()

    def _xValueChanged(self):
        if self.checkSquareScan.isChecked():
            self.spinBCount.setValue(self.spinACount.value())
        if self.checkEqualAspect.isChecked():
            self.spinROIHeight.setValue(self.spinROIWidth.value() / self.spinACount.value() * self.spinBCount.value())
        self.generate_pattern()


class LineScanWidget(ScanWidget, UiWidget):

    def __init__(self):
        super().__init__()


class CircleScanWidget(ScanWidget, UiWidget):

    def __init__(self):
        super().__init__()


class ControlGroupBox(QGroupBox, UiWidget):

    scan = pyqtSignal()
    acquire = pyqtSignal()
    snap = pyqtSignal()
    stop = pyqtSignal()

    def __init__(self):
        super().__init__()

        self.pushScan.pressed.connect(self.scan.emit)
        self.pushStop.pressed.connect(self.stop.emit)
        self.pushAcquire.pressed.connect(self.acquire.emit)
        self.pushSnap.pressed.connect(self.stop.emit)


class ScanGroupBox(QGroupBox, UiWidget):

    update = pyqtSignal()

    def __init__(self):
        super().__init__()

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
            if type(child) in [QSpinBox, QDoubleSpinBox]:
                child.valueChanged.connect(self.update.emit)
            elif type(child) in [QCheckBox]:
                child.stateChanged.connect(self.update.emit)
            elif type(child) in [QRadioButton]:
                child.toggled.connect(self.update.emit)

        self.update.connect(self.ScanWidget.generate_pattern)

        self.pushPreview.pressed.connect(self._preview)

    def _selectSubwidget(self):
        self.replaceWidget(self.ScanWidget, self._subwidgets[self.comboScanPattern.currentText()])
        self.ScanWidget.generate_pattern()

    def _preview(self):
        import matplotlib.pyplot as plt
        plt.plot(self.ScanWidget._pattern.x)
        plt.plot(self.ScanWidget._pattern.y)
        plt.plot(self.ScanWidget._pattern.line_trigger)
        plt.show()



class FileGroupBox(QGroupBox, UiWidget):

    def __init__(self):
        super().__init__()


class ProcessingGroupBox(QGroupBox, UiWidget):

    def __init__(self):
        super().__init__()


class DisplayWidget(QWidget, UiWidget):

    def __init__(self):
        super().__init__()


class SpectrumWidget(QWidget, UiWidget):

    def __init__(self):
        super().__init__()


class MainWindow(QMainWindow, UiWidget):

    def __init__(self):
        super().__init__()

        self.replaceWidget(self.ScanGroupBox, ScanGroupBox())
        self.replaceWidget(self.DisplayWidget, DisplayWidget())
        self.replaceWidget(self.FileGroupBox, FileGroupBox())
        self.replaceWidget(self.SpectrumWidget, SpectrumWidget())
        self.replaceWidget(self.ProcessingGroupBox, ProcessingGroupBox())
        self.replaceWidget(self.ControlGroupBox, ControlGroupBox())

        self.show()

        self.statusBar().setSizeGripEnabled(False)
        self.setFixedSize(self.minimumSize())

