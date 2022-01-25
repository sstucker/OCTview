from PyQt5.QtWidgets import QWidget, QLayout, QGridLayout, QGroupBox, QMainWindow, QSpinBox, QDoubleSpinBox, QCheckBox, QRadioButton, QFileDialog, QLineEdit, QTextEdit, QComboBox, QLabel

import pyqtgraph as pyqtg
import os
from PyQt5 import QtCore
from PyQt5.QtCore import QLine
from PyQt5 import uic
from PyQt5.QtCore import pyqtSignal, QObject

import json

from scanpatterns import LineScanPattern, RasterScanPattern, CircleScanPattern
import numpy as np

class UiWidget:

    def __init__(self, uiname: str = None, statefile: str = None):
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

        if statefile is not None and os.path.exists(statefile):
            self._loadState(statefile)

    def replaceWidget(self, old_widget: QWidget, new_widget: QWidget):
        """Replace a widget with another."""
        plotwidget_placeholder_layout = old_widget.parent().layout()
        plotwidget_placeholder_layout.replaceWidget(old_widget, new_widget)
        new_widget.setObjectName(old_widget.objectName())
        old_widget.hide()
        new_widget.show()
        self.__dict__[old_widget.objectName()] = new_widget  # Goodbye, old widget reference!

    def loadStateFromJson(self, statefile: str):
        with open(statefile, 'r') as file:
            print('Opened the .json file.')

    def writeStateToJson(self, statefile: str):
        with open(statefile, 'w') as file:
            print(json.dumps(_dictize(self), indent=4))
            json.dump(_dictize(self), file, indent=4)


DICTABLE_TYPES = [
    QComboBox,
    QSpinBox,
    QDoubleSpinBox,
    QCheckBox,
    QRadioButton,
    QLineEdit,
    QTextEdit,
]


def _dictize(obj: QWidget):
    """Convert a hierarchy of QWidgets and QLayouts to a dictionary

    Loops over entire tree given a root object. Will only assign a QWidget or QLayout's name and value to the dictionary
    if it is in DICTABLE_TYPES, therefore having behavior defined in _get_widget_value.

    Args:
        obj (QWidget or QLayout): Qt Widget or Layout instance to serve as the root of the tree
    """
    struct = {}
    for child in obj.children():
        if type(child) in DICTABLE_TYPES and not child.objectName().startswith('qt_'):
            struct[child.objectName()] = _get_widget_value(child)
        elif len(child.children()) > 0:
            # Recurse if children have dictable children
            s = _dictize(child)
            if len(s) > 0:
                struct[child.objectName()] = s
    return struct


def _get_widget_value(widget: QWidget):
    if type(widget) in [QCheckBox, QRadioButton]:
        return widget.isChecked()
    elif type(widget) in [QLineEdit, QTextEdit]:
        return widget.text()
    elif type(widget) in [QSpinBox, QDoubleSpinBox]:
        return widget.value()
    elif type(widget) in [QComboBox]:
        return widget.currentIndex()


def _set_widget_value(widget: QWidget, value):
    if type(widget) in [QCheckBox, QRadioButton]:
        widget.setChecked(value)
    elif type(widget) in [QLineEdit, QTextEdit]:
        widget.setText(value)
    elif type(widget) in [QSpinBox, QDoubleSpinBox]:
        widget.setValue(value)
    elif type(widget) in [QComboBox]:
        widget.setCurrentIndex(value)


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
        return self._pattern.x

    @property
    def y(self):
        return self._pattern.y

    @property
    def line_trigger(self):
        return self._pattern.line_trigger

    @property
    def frame_trigger(self):
        return self._pattern.frame_trigger

    @property
    def pattern(self):
        return self._pattern



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
        self.radioXSaw.toggled.connect(self._profileChanged)

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
            self.labelROIHeight.setEnabled(False)
        else:
            self.spinROIHeight.setEnabled(True)
            self.labelROIHeight.setEnabled(True)
        if self.checkSquareScan.isChecked():
            self.spinBCount.setValue(self.spinACount.value())
            self.spinBCount.setEnabled(False)
            self.labelBCount.setEnabled(False)
        else:
            self.spinBCount.setEnabled(True)
            self.labelBCount.setEnabled(True)
        self.generate_pattern()

    def _checkBidirectionalChanged(self):
        if self.checkBidirectional.isChecked():
            self.checkARepeat.setChecked(False)
            self.checkBRepeat.setChecked(False)
            self.radioXSaw.setChecked(True)
            self.spinFlybackDuty.setEnabled(False)
            self.labelFlybackDuty.setEnabled(False)
        else:
            self.spinFlybackDuty.setEnabled(True)
            self.labelFlybackDuty.setEnabled(True)

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

    def _profileChanged(self):
        if self.radioXStep.isChecked():
            self.spinExposureFraction.setEnabled(False)
            self.labelExposureFraction.setEnabled(False)
        else:
            self.spinExposureFraction.setEnabled(True)
            self.labelExposureFraction.setEnabled(True)
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
        plt.plot(self.x)
        plt.plot(self.y)
        plt.plot(self.line_trigger)
        plt.show()

    @property
    def x(self):
        return self.pattern.x

    @property
    def y(self):
        return self.pattern.y

    @property
    def line_trigger(self):
        return self.pattern.line_trigger

    @property
    def frame_trigger(self):
        return self.pattern.frame_trigger

    @property
    def zroi(self):
        return self.spinZTop.value(), self.spinZBottom.value()

    @property
    def pattern(self):
        return self.ScanWidget.pattern


class FileGroupBox(QGroupBox, UiWidget):

    def __init__(self):
        super().__init__()
        self._directory: str = os.getcwd()
        self._file_name: str = 'default_trial_name'

        self.lineDirectory.setText(self._directory)
        self.lineFileName.setText(self._file_name)

        self.buttonBrowse.pressed.connect(self._browseForDirectory)

    def _browseForDirectory(self):
        self._directory = str(QFileDialog.getExistingDirectory(self, "Select Experiment Directory", self._directory))
        self.lineDirectory.setText(self._directory)

    @property
    def directory(self):
        return self._directory

    @property
    def trial(self):
        return self._file_name

    @property
    def filename(self):
        return os.path.join(self.directory, self.trial)


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

        self.statusBar().setSizeGripEnabled(False)
        self.setFixedSize(self.minimumSize())

        self.show()

        self.writeStateToJson('teststate.json')

"""
Parameters

DAC_ID_AO_OUT_GALVO_X
DAC_ID_AO_OUT_GALVO_Y
DAC_ID_AO_OUT_LINE_TRIGGER
DAC_ID_AO_FRAME_TRIGGER
DAC_ID_AO_START_TRIGGER
DAC_ID_AI_START_TRIGGER
CAM_ID

SCAN_SIGNAL_X
SCAN_SIGNAL_Y
SCAN_SIGNAL_LINE_TRIGGER
SCAN_SIGNAL_FRAME_TRIGGER
SCAN_SIGNAL_SAMPLE_RATE

EXPORT_FILE_TYPE
EXPORT_FILE_NAME
EXPORT_FILE_SIZE
EXPORT_NUMBER_OF_FRAMES











"""

