import json
import numpy as np
import os
import pyqtgraph
import pyqtgraph.opengl
import warnings
from PyQt5 import uic
from PyQt5.QtCore import pyqtSignal, QTimer
from PyQt5.QtWidgets import QWidget, QLayout, QGridLayout, QGroupBox, QMainWindow, QSpinBox, QDoubleSpinBox, QCheckBox, \
    QRadioButton, \
    QFileDialog, QMessageBox, QLineEdit, QTextEdit, QComboBox, QDialog, QFrame, QApplication
from pyqtgraph.graphicsItems.InfiniteLine import InfiniteLine as pyqtgraphSlider
from scanpatterns import LineScanPattern, RasterScanPattern
from threading import Thread, Lock
from queue import Queue
import copy

import OCTview

def replaceWidget(old_widget: QWidget, new_widget: QWidget):
    """Replace a widget with another."""
    plotwidget_placeholder_layout = old_widget.parent().layout()
    plotwidget_placeholder_layout.replaceWidget(old_widget, new_widget)
    new_widget.setObjectName(old_widget.objectName())
    old_widget.hide()
    new_widget.show()
    old_widget.parentWidget().__dict__[old_widget.objectName()] = new_widget  # Goodbye, old widget reference!


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
        ui = os.sep.join([OCTview.ui_resource_location, uiname + '.ui'])
        uic.loadUi(ui, self)

    def loadStateFromJson(self, statefile: str):
        with open(statefile, 'r') as file:
            try:
                _undictize(self, json.load(file)[self.objectName()])
            except KeyError:
                raise ValueError(self.__class__.__name__
                                 + " instance named '" + self.objectName()
                                 + "' not found the root of state file " + statefile)

    def writeStateToJson(self, statefile: str):
        with open(statefile, 'w') as file:
            json.dump({self.objectName(): _dictize(self)}, file, indent=4)


# These Widget types will be dictized and undictized by the functions below
DICTABLE_TYPES = [
    QComboBox,
    QSpinBox,
    QDoubleSpinBox,
    QCheckBox,
    QRadioButton,
    QLineEdit,
    QTextEdit,
]


def _undictize(obj, dictionary: dict):
    """Read values and states of QWidgets from a nested dictionary of object names.

    Recurses over entire dictionary tree, assigning values to children of obj.

    Args:
        obj (QWidget or QLayout): Qt Widget or Layout instance to serve as the root of the tree
        dictionary (dict): Dictionary with keys corresponding to children of obj
    """
    for key in dictionary.keys():
        try:
            child = getattr(obj, key)
        except AttributeError:  # For some reason, not all children are in __dict__
            child = None
            for c in obj.children():
                if key == c.objectName():
                    child = c
                    break
            if child is None:
                warnings.warn(key + ' not found in ' + obj.objectName())
                continue
        if type(child) in DICTABLE_TYPES:
            _set_widget_value(child, dictionary[key])
        else:
            _undictize(child, dictionary[key])


def _dictize(obj):
    """Convert a hierarchy of QWidgets and QLayouts to a dictionary.

    Recurses over entire tree given a root object. Will only assign a QWidget or QLayout's name and value to the dictionary
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
    elif type(widget) in [QLineEdit]:
        return widget.text()
    elif type(widget) in [QTextEdit]:
        return widget.toPlainText()
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


def _connect_all_child_widgets_to_slot(parent_widget: QWidget, slot: callable, recurse=True):
    for child in parent_widget.children():
        if recurse:
            if type(child) in [QWidget, QFrame, QGroupBox, QDialog]:
                _connect_all_child_widgets_to_slot(child, slot, recurse=True)
        if type(child) in [QSpinBox, QDoubleSpinBox]:
            child.valueChanged.connect(slot)
        elif type(child) in [QCheckBox]:
            child.stateChanged.connect(slot)
        elif type(child) in [QRadioButton]:
            child.toggled.connect(slot)


class ScanWidget(QWidget):

    def __init__(self):
        super().__init__()
        self._pattern = LineScanPattern()

    def generate_pattern(self):
        raise NotImplementedError()

    def x(self):
        return self._pattern.x

    def y(self):
        return self._pattern.y

    def a_repeats(self) -> int:
        raise NotImplementedError()

    def b_repeats(self) -> int:
        raise NotImplementedError()

    def total_alines(self) -> int:
        raise NotImplementedError()

    def line_trigger(self):
        return self._pattern.line_trigger

    def frame_trigger(self):
        return self._pattern.frame_trigger

    def pattern(self):
        return self._pattern

    def fixSize(self, fix: bool):
        """Disable all controls that allow the size of the image to be changed."""
        raise NotImplementedError()


class RasterScanWidget(ScanWidget, UiWidget):
    pattern_updated = pyqtSignal()  # Reports new pattern assigned to clients

    def __init__(self):
        super().__init__()

        # TODO implement bidirectional patterns
        self.checkBidirectional.setVisible(False)

        self.checkSquareScan.stateChanged.connect(self._aspectChanged)
        self.checkEqualAspect.stateChanged.connect(self._aspectChanged)
        self.checkBidirectional.stateChanged.connect(self._checkBidirectionalChanged)
        self.checkARepeat.stateChanged.connect(self._checkARepeatChanged)
        self.checkBRepeat.stateChanged.connect(self._checkBRepeatChanged)
        self.spinACount.valueChanged.connect(self._valueChanged)
        self.spinROIWidth.valueChanged.connect(self._valueChanged)
        self.spinBCount.valueChanged.connect(self._valueChanged)
        self.spinROIHeight.valueChanged.connect(self._valueChanged)
        self.radioXSaw.toggled.connect(self._profileChanged)

        self._settings_dialog = RasterScanDialog()
        self._settings_dialog.setParent(self)
        self._settings_dialog.setWindowFlag(True)

        self.buttonSettings.pressed.connect(self._settings_dialog.showDialog)

        self._pattern = RasterScanPattern()

        # Debounce timer
        self._timer_generate = QTimer()
        self._ctr_generate = 0

        self._pattern_mtx = Lock()
        self._gen_thread = Thread()
        self._gen_queue = Queue()

    def _gen_worker(self):
        while self._gen_queue.qsize() > 0:
            while self._gen_queue.qsize() > 1:
                self._gen_queue.get()  # pop first in jobs we don't care about
            with self._pattern_mtx:
                self._pattern.generate(**self._gen_queue.get())
            self.parentWidget().linePatternRate.setText(str(self._pattern.pattern_rate)[0:5] + ' Hz')
        self.pattern_updated.emit()

    def generate_pattern(self):
        self._ctr_generate += 1
        self._timer_generate.singleShot(OCTview.CALLBACK_DEBOUNCE_MS, self._generate_pattern)

    def _generate_pattern(self):
        self._ctr_generate -= 1
        if not self._ctr_generate > 0:
            p = {
                'alines': self.spinACount.value(),
                'blines': self.spinBCount.value(),
                'max_trigger_rate': self.parentWidget().parentWidget().parentWidget().max_line_rate(),  # MainWindow
                # Pattern gen in millimeters!
                'fov': [self.spinROIWidth.value() * 0.001, self.spinROIHeight.value() * 0.001],
                'flyback_duty': self.spinFlybackDuty.value() / 100,
                'exposure_fraction': self.spinExposureFraction.value() / 100,
                'fast_axis_step': self.radioXStep.isChecked(),
                'slow_axis_step': self.radioYStep.isChecked(),
                'aline_repeat': [1, self.spinARepeat.value()][int(self.checkARepeat.isChecked())],
                'bline_repeat': [1, self.spinBRepeat.value()][int(self.checkBRepeat.isChecked())],
                'bidirectional': self.checkBidirectional.isChecked(),
                'rotation_rad': self.parentWidget().spinRotation.value() * np.pi / 180,
                'trigger_blines': self.parentWidget().parentWidget().parentWidget().blines_triggered(),
                'samples_on': self._settings_dialog.spinSamplesOn.value(),
                'samples_off': self._settings_dialog.spinSamplesOff.value()
            }
            self._gen_queue.put(p)
            if not self._gen_thread.is_alive():
                self._gen_thread = Thread(target=self._gen_worker)
                self._gen_thread.start()


    def fixSize(self, fixed: bool):
        """Disables all widgets that allow the editing of the image size. This cannot be changed during a scan.

        Args:
            fixed (bool): if True, widgets are disabled.
        """
        if self.checkSquareScan.isChecked():
            self.spinBCount.setEnabled(False)
            self.labelBCount.setEnabled(False)
        else:
            self.spinBCount.setEnabled(not fixed)
            self.labelBCount.setEnabled(not fixed)
        self.spinARepeat.setEnabled(not fixed)
        self.spinBRepeat.setEnabled(not fixed)
        self.checkARepeat.setEnabled(not fixed)
        self.checkBRepeat.setEnabled(not fixed)
        self.spinACount.setEnabled(not fixed)
        self.checkSquareScan.setEnabled(not fixed)
        self.buttonSettings.setEnabled(not fixed)

    def total_alines(self) -> int:
        return self.spinACount.value() * self.spinBCount() * self.spinARepeat.value() * self.spinBRepeat.value()

    def a_repeats(self):
        if self.checkARepeat.isChecked():
            return self.spinARepeat.value()
        else:
            return 0

    def b_repeats(self):
        if self.checkBRepeat.isChecked():
            return self.spinBRepeat.value()
        else:
            return 0

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
        self._valueChanged()

    def _checkBidirectionalChanged(self):
        if self.checkBidirectional.isChecked():
            self.checkARepeat.setChecked(False)
            self.checkBRepeat.setChecked(False)
            self.radioXSaw.setChecked(True)
            self.spinFlybackDuty.setEnabled(False)
            self.labelFlybackDuty.setEnabled(False)
            self.radioXStep.setChecked(False)
        else:
            self.spinFlybackDuty.setEnabled(True)
            self.labelFlybackDuty.setEnabled(True)

    def _checkARepeatChanged(self):
        if self.checkARepeat.isChecked():
            self.checkBidirectional.setChecked(False)
            self.spinARepeat.setEnabled(True)
        else:
            self.spinARepeat.setEnabled(False)

    def _checkBRepeatChanged(self):
        if self.checkBRepeat.isChecked():
            self.checkBidirectional.setChecked(False)
            self.spinBRepeat.setEnabled(True)
        else:
            self.spinBRepeat.setEnabled(False)

    def _valueChanged(self):
        if self.checkSquareScan.isChecked():
            self.spinBCount.setValue(self.spinACount.value())
        if self.checkEqualAspect.isChecked():
            self.spinROIHeight.setValue(self.spinROIWidth.value() / self.spinACount.value() * self.spinBCount.value())

    def _profileChanged(self):
        self.spinExposureFraction.setEnabled(not self.radioXStep.isChecked())
        self.labelExposureFraction.setEnabled(not self.radioXStep.isChecked())

    # Overload
    def pattern(self) -> RasterScanPattern:
        """Pattern generation occurs in a thread, so we need to get a lock and make a deep copy"""
        # self._pattern_mtx.acquire(blocking=False)
        # p = copy.copy(self._pattern)
        # self._pattern_mtx.release()
        with self._pattern_mtx:
            p = copy.copy(self._pattern)
        return p


class LineScanWidget(ScanWidget, UiWidget):

    def __init__(self):
        super().__init__()


class CircleScanWidget(ScanWidget, UiWidget):

    def __init__(self):
        super().__init__()


class ScanGroupBox(QGroupBox, UiWidget):
    changed = pyqtSignal()

    def __init__(self):
        super().__init__()

        self._subwidgets = {
            'Raster': RasterScanWidget(),
            'Figure-8': LineScanWidget(),
            'Circle': CircleScanWidget()
        }
        self._selectSubwidget()
        self.comboScanPattern.currentIndexChanged.connect(self._selectSubwidget)

        # All widgets should emit the scan pattern changed signal
        _connect_all_child_widgets_to_slot(self, self.ScanWidget.generate_pattern)
        for subwidget in list(self._subwidgets.values()):
            _connect_all_child_widgets_to_slot(subwidget, self.ScanWidget.generate_pattern)

        self.ScanWidget.pattern_updated.connect(self._update_preview)
        self.ScanWidget.pattern_updated.connect(self.changed.emit)

        self.pushPreview.pressed.connect(self._preview)

        self._preview_window = ScanDisplayWindow()
        self._preview_window.setParent(self)
        self._preview_window.setWindowFlag(True)

    def _selectSubwidget(self):
        replaceWidget(self.ScanWidget, self._subwidgets[self.comboScanPattern.currentText()])
        self.ScanWidget.generate_pattern()

    def _update_preview(self):
        if self._preview_window.isVisible():
            self._preview_window.previewPattern(self.pattern())

    def _preview(self):
        if not self._preview_window.isVisible():
            self._preview_window.previewPattern(self.pattern())

    def toggleScanningMode(self, scanning_mode: bool):
        self.ScanWidget.fixSize(scanning_mode)
        self.spinZTop.setEnabled(not scanning_mode)
        self.labelZTop.setEnabled(not scanning_mode)
        self.spinZBottom.setEnabled(not scanning_mode)
        self.labelZBottom.setEnabled(not scanning_mode)
        self.comboScanPattern.setEnabled(not scanning_mode)
        self.labelScanPatternType.setEnabled(not scanning_mode)

    def x(self):
        return self.pattern().x

    def y(self):
        return self.pattern().y

    def line_trigger(self):
        return self.pattern().line_trigger

    def frame_trigger(self):
        return self.pattern().frame_trigger

    def zroi(self):
        self.spinZBottom.setMinimum(self.spinZTop.value() + 1)
        self.spinZTop.setMaximum(self.spinZBottom.value() - 1)
        return self.spinZTop.value(), self.spinZBottom.value()

    def pattern(self):
        return self.ScanWidget.pattern()

    def a_repeats(self):
        return self.ScanWidget.a_repeats()

    def b_repeats(self):
        return self.ScanWidget.b_repeats()

    def rotation_degrees(self) -> float:
        return self.spinRotation.value()

    def x_offset(self) -> float:
        """Scan offset in mm"""
        return self.spinXOffset.value() * 0.001

    def y_offset(self) -> float:
        """Scan offset in mm"""
        return self.spinYOffset.value() * 0.001

class ControlGroupBox(QGroupBox, UiWidget):
    scan = pyqtSignal()
    acquire = pyqtSignal()
    stop = pyqtSignal()

    def __init__(self):
        super().__init__()

        self.pushScan.pressed.connect(self.set_mode_not_ready)
        self.pushScan.pressed.connect(self.scan.emit)
        self.pushStop.pressed.connect(self.set_mode_not_ready)
        self.pushStop.pressed.connect(self.stop.emit)
        self.pushAcquire.pressed.connect(self.set_mode_not_ready)
        self.pushAcquire.pressed.connect(self.acquire.emit)

        self.checkNumberedAcquisition.toggled.connect(self._numbered_check_changed)
        self.spinFramesToAcquire.valueChanged.connect(self._frames_to_acquire_changed)

        self.set_mode_not_ready()

    def set_mode_not_ready(self):
        self.pushScan.setEnabled(False)
        self.pushAcquire.setEnabled(False)
        self.pushStop.setEnabled(False)
        self.spinFramesToAcquire.setEnabled(False)
        self.checkNumberedAcquisition.setEnabled(False)

    def set_mode_scanning(self):
        self.pushScan.setEnabled(False)
        self.pushAcquire.setEnabled(True)
        self.pushStop.setEnabled(True)
        self._numbered_check_changed()
        self.checkNumberedAcquisition.setEnabled(True)

    def set_mode_acquiring(self):
        self.pushScan.setEnabled(True)
        self.pushAcquire.setEnabled(False)
        self.pushStop.setEnabled(True)
        self.spinFramesToAcquire.setEnabled(False)
        self.checkNumberedAcquisition.setEnabled(False)

    def set_mode_ready(self):
        self.pushScan.setEnabled(True)
        self.pushAcquire.setEnabled(True)
        self.pushStop.setEnabled(False)
        self._numbered_check_changed()
        self.checkNumberedAcquisition.setEnabled(True)

    def _numbered_check_changed(self):
        self.spinFramesToAcquire.setEnabled(self.checkNumberedAcquisition.isChecked())

    def _frames_to_acquire_changed(self):
        if self.spinFramesToAcquire.value() > 1:
            self.spinFramesToAcquire.setSuffix(' frames')
        else:
            self.spinFramesToAcquire.setSuffix(' frame')

    def frames_to_acquire(self):
        if self.checkNumberedAcquisition.isChecked():
            return self.spinFramesToAcquire.value()
        else:
            return -1


class ScanDisplayWindow(QFrame):

    def __init__(self):
        super().__init__()

        self.hide()

        self._graphics_window = pyqtgraph.GraphicsWindow()
        # self._signal_plot = self._graphics_window.addPlot(row=0, col=0, rowspan=1, colspan=2)

        # self._line_trigger_item = self._signal_plot.plot()
        # self._frame_trigger_item = self._signal_plot.plot()
        # self._x_item = self._signal_plot.plot()
        # self._y_item = self._signal_plot.plot()
        # self._trigger_legend = self._signal_plot.addLegend()

        self._scan_plot = self._graphics_window.addPlot(row=1, col=0, rowspan=2, colspan=2)
        self._scan_item = self._scan_plot.plot()
        self._scan_points_item = self._scan_plot.plot()
        self._scan_plot.setAspectLocked()

        self._layout = QGridLayout()
        self._layout.addWidget(self._graphics_window)
        self.setLayout(self._layout)

    def previewPattern(self, pattern: LineScanPattern):
        # t = np.arange(len(pattern.x)) * 1 / pattern.sample_rate
        # # self._line_trigger_item.setData(x=t, y=pattern.line_trigger, name='Line trigger')
        # self._frame_trigger_item.setData(x=t, y=pattern.frame_trigger, name='Frame trigger')
        # self._x_item.setData(x=t, y=pattern.x, name='x')
        # self._y_item.setData(x=t, y=pattern.y, name='y')

        exp = np.zeros(len(pattern.x)).astype(np.int32)
        exp[pattern.line_trigger.astype(bool)] = 1
        scan_x = pattern.x + self.scan_offset_mm_x()
        scan_y = pattern.y + self.scan_offset_mm_y()
        x_pts = pattern.positions[:, 0]
        y_pts = pattern.positions[:, 1]
        self._scan_item.setData(x=pattern.x, y=pattern.y, pen=pyqtgraph.mkPen(width=1, color='#FEFEFE'),
                                antialias=True, autodownsample=True, skipFiniteCheck=True)
        self._scan_points_item.setData(x=x_pts, y=y_pts,
                                       pen=None, symbol='o', symbolSize=2, antialias=False,
                                       autodownsample=True, skipFiniteCheck=True)

        self.show()


class FileGroupBox(QGroupBox, UiWidget):

    def __init__(self):
        super().__init__()

        self._directory: str = os.getcwd()
        self._file_name: str = 'default_trial_name'

        self.lineDirectory.setText(self._directory)
        self.lineFileName.setText(self._file_name)

        self.buttonBrowse.pressed.connect(self._browseForDirectory)

    def _browseForDirectory(self):
        self.lineDirectory.setText(str(QFileDialog.getExistingDirectory(self, "Select Experiment Directory", self.lineDirectory.text())))

    def directory(self) -> str:
        return os.path.normpath(self.lineDirectory.text())

    def trial(self) -> str:
        return "".join(c for c in self.lineFileName.text() if c.isalnum() or c in "_- ")

    def filename(self):
        return os.path.join(self.directory(), self.trial())

    def save_metadata(self):
        return self.checkMetadata.isChecked()

    def save_processed(self) -> bool:
        return self.radioProcessed.isChecked()

    def max_gb(self) -> float:
        txt = self.comboFileSize.currentText()
        units = {
            'MB': 0.001,
            'GB': 1.0,
        }
        return units[txt.split(' ')[1]] * float(int(txt.split(' ')[0]))


class ProcessingGroupBox(QGroupBox, UiWidget):

    changed = pyqtSignal()

    def __init__(self):
        super().__init__()

        self._windows = {
            'Hanning': np.hanning,
            'Blackman': np.blackman
        }
        self._null_window = np.ones

        self.checkApodization.toggled.connect(self._apodToggled)
        self.checkInterpolation.toggled.connect(self._interpToggled)
        self.radioFrameNone.toggled.connect(self._frameProcessingToggled)

        self.groupFrameProcessing.setVisible(False)  # TODO implement frame averaging in the backend

        _connect_all_child_widgets_to_slot(self, self.changed.emit)

    def _apodToggled(self):
        self.labelApodization.setEnabled(self.checkApodization.isChecked())
        self.comboApodization.setEnabled(self.checkApodization.isChecked())

    def _interpToggled(self):
        self.labelInterpDk.setEnabled(self.checkInterpolation.isChecked())
        self.spinInterpDk.setEnabled(self.checkInterpolation.isChecked())

    def _frameProcessingToggled(self):
        self.spinFrameAverage.setEnabled(self.radioFrameAverage.isChecked())

    def setARepeatProcessingDisplay(self, rpt: int):
        self.groupARepeatProcessing.setVisible(rpt > 1)

    def setBRepeatProcessingDisplay(self, rpt: int):
        self.groupBRepeatProcessing.setVisible(rpt > 1)
        self.radioBRepeatDifference.setVisible(rpt == 2)

    def toggleScanningMode(self, scanning: bool):
        self.groupFrameProcessing.setEnabled(not scanning)
        self.groupARepeatProcessing.setEnabled(not scanning)
        self.groupBRepeatProcessing.setEnabled(not scanning)

    def background_subtraction(self):
        return self.checkMeanSpectrumSubtraction.isChecked()

    def interpolation(self):
        return self.checkInterpolation.isChecked()

    def interpdk(self):
        return self.spinInterpDk.value()

    def apodization_window(self) -> callable:
        """Returns function which defines the apodization window. Takes A-line size as an argument."""
        if self.checkApodization.isChecked():
            return self._windows[self.comboApodization.currentText()]
        else:
            return self._null_window

    def a_repeat_processing(self):
        if self.radioARepeatAverage.isChecked():
            return 'average'
        else:
            return None

    def b_repeat_processing(self):
        if self.radioBRepeatAverage.isChecked():
            return 'average'
        elif self.radioBRepeatDifference.isChecked():
            return 'difference'
        else:
            return None

    def frame_averaging(self):
        if not self.radioFrameAverage.isChecked():
            return 0
        else:
            return self.spinFrameAverage.value()


class SpectrumPlotWidget(pyqtgraph.GraphicsWindow):
    yrange = property()
    wavelengths = property()

    def __init__(self, wavelengths=np.arange(0, 2048), yrange=[0, 4096]):
        super().__init__()

        # self.resize(200, 100)
        # self.setFixedSize(self.minimumSize())

        self._wavelengths = wavelengths
        self._n = len(self._wavelengths)

        self._plot = self.addPlot()
        self._plot.buttonsHidden = True
        self._plot.setYRange(yrange[0], yrange[1])
        self._plot.setLabel('bottom', text='λ (nm)')

        self._spectrum = self._plot.plot(color='#FFFFFF')

        self._current_data = 0
        self.plot(np.zeros(self._n))

    def plot(self, data, wavelengths=None):
        if wavelengths is None:
            wavelengths = self._wavelengths
        self._current_data = data
        self._spectrum.setData(wavelengths, self._current_data)

    @yrange.setter
    def yrange(self, yrange):
        self.plot.setYRange(yrange[0], yrange[1])

    @wavelengths.setter
    def wavelengths(self, wavelengths):
        self._wavelengths = wavelengths
        self._n = len(self._wavelengths)


class VolumeWidget(pyqtgraph.opengl.GLViewWidget):

    def __init__(self):
        super().__init__()
        # self._grid = pyqtgraph.opengl.GLGridItem()
        self._axis = pyqtgraph.opengl.GLAxisItem()

        # self.setFixedSize(self.minimumSize())
        # self.resize(100, 100)

        img = np.random.random([128, 128, 200])
        data = np.empty(img.shape + (4,), dtype=np.ubyte)
        data[..., 0] = img * (255. / (img.max() / 1))
        data[..., 3] = (data[..., 3].astype(float) / 255.) ** 2 * 255

        self._volume = pyqtgraph.opengl.GLVolumeItem(data, sliceDensity=1, smooth=True, glOptions='translucent')

        self.opts['distance'] = 600
        # self.addItem(self._grid)
        self.addItem(self._axis)
        self.addItem(self._volume)

    def updateData(self, volume):
        extent = volume.shape * 3
        # self._grid.setSize(x=extent[0], y=extent[1], z=extent[2])
        self._axis.setSize(x=extent[0], y=extent[1], z=extent[2])

        data = np.empty(volume.shape + (4,), dtype=np.ubyte)
        data[..., 0] = volume * (255. / (volume.max() / 1))
        data[..., 3] = (data[..., 3].astype(float) / 255.) ** 2 * 255
        self._volume.setData(data)


class BScanWidget(pyqtgraph.GraphicsLayoutWidget):

    def __init__(self, title=None, hslider=False, vslider=False):
        super().__init__()

        self._plot = self.addPlot()
        self.setFixedSize(340, 340)

        self._slice_not_set = True

        self._plot.setAspectLocked(True)
        self._plot.showGrid(x=True, y=True)
        self._plot.invertY()
        self._plot.setLabel('left', units='m')
        self._plot.setLabel('bottom', units='m')
        self._image = pyqtgraph.ImageItem()
        self._plot.addItem(self._image)

        if hslider:
            self._hslider = pyqtgraphSlider(movable=True, angle=0)
            self._hslider.setPen(width=2, color='#FFFF0099')
            self._hslider.setHoverPen(width=3, color='#FFFFFF99')
            self._hslider.sigPositionChanged.connect(self._setSlice)
            self._hslider.setZValue(1)
            self._plot.addItem(self._hslider)
        else:
            self._hslider = None

        if vslider:
            self._vslider = pyqtgraphSlider(movable=True, angle=90)
            self._vslider.setPen(width=2, color='#FFFF0099')
            self._vslider.setHoverPen(width=3, color='#FFFFFF99')
            self._vslider.sigPositionChanged.connect(self._setSlice)
            self._vslider.setZValue(1)
            self._plot.addItem(self._vslider)
        else:
            self._vslider = None

        self._vslice = None
        self._hslice = None

        self._sf = [1, 1]
        self._data_shape = [0, 0]

    def unset(self):
        self._slice_not_set = True

    def setVSliderHidden(self, hidden: bool):
        if self._vslider is not None:
            if hidden:
                self._vslider.hide()
            else:
                self._vslider.show()

    def setHSliderHidden(self, hidden: bool):
        if self._hslider is not None:
            if hidden:
                self._hslider.hide()
            else:
                self._hslider.show()

    def setAspect(self, dx, dy):
        self._image.scale(1 / self._sf[0], 1 / self._sf[1])  # Undo any previous scaling
        sfx = (1 / self._data_shape[0]) * dx
        sfy = (1 / self._data_shape[1]) * dy
        if sfx <= 0 or sfy <= 0:
            self._sf = [1.0, 1.0]
        else:
            self._sf = [sfx, sfy]
        if self._vslider is not None:
            self._vslider.setBounds([0, self._sf[0] * self._data_shape[0]])
        if self._hslider is not None:
            self._hslider.setBounds([0, self._sf[1] * self._data_shape[1]])
        self._image.scale(self._sf[0], self._sf[1])

    def updateData(self, image, fov=None):
        self._data_shape = image.shape
        if fov is not None:
            self.setAspect(fov[0], fov[1])

        if np.isnan(np.sum(image)):
            print('OCTview: NaN encountered in image enqueued for display')
            return

        if image.size > 64 * 64:
            n = int(image.size / 4)  # TODO make this smarter
        else:
            n = image.size
        # pyqtgraph auto levels doesn't work for images >= 2**16
        self._image.setImage(image, levels=[np.nanmin(image),
                                            np.nanmax(image)])
        if self._slice_not_set:
            self._setSlice()
            self._slice_not_set = False

    def _setSlice(self):
        if self._hslider is not None:
            self._hslice = int(self._hslider.value() / self._sf[1])
            if self._hslice > self._data_shape[1] - 1:
                self._hslice = self._data_shape[1] - 1
        if self._vslider is not None:
            self._vslice = int(self._vslider.value() / self._sf[0])
            if self._vslice > self._data_shape[0] - 1:
                self._vslice = self._data_shape[0] - 1

    def setSlice(self):
        self._setSlice()

    @property
    def hslice(self):
        return self._hslice

    @property
    def vslice(self):
        return self._vslice


class DisplayWidget(QWidget, UiWidget):

    def __init__(self):
        super().__init__()

        self._volume = VolumeWidget()
        self._enface = BScanWidget(title='Enface view', hslider=True, vslider=True)
        self._enface.setVSliderHidden(True)
        self._bscan = BScanWidget(title='B-scan view', hslider=True)

        replaceWidget(self.VolumeWidget, self._volume)
        replaceWidget(self.EnfaceWidget, self._enface)
        replaceWidget(self.BScanWidget, self._bscan)

        self.checkEnfaceProjection.toggled.connect(self._enfaceProjectionCheckChanged)
        self.checkBScanProjection.toggled.connect(self._updateEnfaceSliders)
        self.radioViewY.toggled.connect(self._updateEnfaceSliders)
        self._updateEnfaceSliders()
        self._enfaceProjectionCheckChanged()

    def _enfaceProjectionCheckChanged(self):
        self._bscan.setHSliderHidden(self.checkEnfaceProjection.isChecked())
        self.radioEnfaceMax.setEnabled(self.checkEnfaceProjection.isChecked())
        self.radioEnfaceMean.setEnabled(self.checkEnfaceProjection.isChecked())

    def _updateEnfaceSliders(self):
        self.radioBScanMax.setEnabled(self.checkBScanProjection.isChecked())
        self.radioBScanMean.setEnabled(self.checkBScanProjection.isChecked())
        if self.radioViewX.isChecked():
            self._enface.setVSliderHidden(True)
            self._enface.setHSliderHidden(self.checkBScanProjection.isChecked())
        else:
            self._enface.setVSliderHidden(self.checkBScanProjection.isChecked())
            self._enface.setHSliderHidden(True)

    def display_frame(self, frame: np.ndarray, fov=[1, 1, 1]):
        """Display a 3D volume via the B-scan and enface viewers.

        Args:
            frame (np.ndarray): 3D or 2D complex frame
            fov: Dimensions in meters of the frame volume or area.
        """
        if frame.ndim < 3:
            raise IndexError("Only 3D frames can be displayed by DisplayWidget")
        if self.tabDisplay.currentIndex() == 0:  # If in 2D slicing mode
            if self.checkEnfaceProjection.isChecked():
                if self.radioEnfaceMax.isChecked():
                    f = np.max(np.abs(frame), axis=0)
                else:
                    f = np.mean(np.abs(frame), axis=0)
            else:
                f = np.abs(frame[self._bscan.hslice, :, :])
            if self.checkDb.isChecked():
                f = 20 * np.log10(f)
            self._enface.updateData(f, fov=(fov[1], fov[2]))
            if self.radioViewX.isChecked():
                if self.checkBScanProjection.isChecked():
                    if self.radioBScanMax.isChecked():
                        f = np.max(np.abs(frame), axis=2)
                    else:
                        f = np.mean(np.abs(frame), axis=2)
                else:
                    try:
                        f = np.abs(frame[:, :, self._enface.hslice])
                    except IndexError:
                        f = np.abs(frame[:, :, 0])
                        self._enface.unset()

                dim = (fov[1], fov[0])
            else:  # if X
                if self.checkBScanProjection.isChecked():
                    if self.radioBScanMax.isChecked():
                        f = np.max(np.abs(frame), axis=1)
                    else:
                        f = np.mean(np.abs(frame), axis=1)
                else:
                    try:
                        f = np.abs(frame[:, self._enface.vslice, :])
                    except IndexError:
                        f = np.abs(frame[:, 0, :])
                        self._enface.unset()
                dim = (fov[2], fov[0])
            if self.checkDb.isChecked():
                f = 20 * np.log10(f)
            self._bscan.updateData(np.rot90(f), fov=dim)
        elif self.tabDisplay.currentIndex() == 1:  # If in volumetric render mode
            self._volume.updateData(np.abs(frame))
        # pyqtgraph.QtGui.QApplication.processEvents()


class SpectrumWidget(QWidget, UiWidget):

    def __init__(self):
        super().__init__()
        replaceWidget(self.SpectrumPlotWidget, SpectrumPlotWidget())
        self.setMaximumHeight(200)

    def plot(self, wavelengths: np.ndarray, spectrum: np.ndarray):
        self.SpectrumPlotWidget.plot(spectrum, wavelengths=wavelengths)


class CancelDiscardsChangesDialog(QDialog, UiWidget):
    changed = pyqtSignal()

    def __init__(self):
        super().__init__()

    def showDialog(self):
        old_state = _dictize(self)
        if not bool(self.exec_()):
            _undictize(self, old_state)
        else:
            # TODO actually compare the new state with old_state before emitting this
            self.changed.emit()


class SettingsDialog(CancelDiscardsChangesDialog):

    def __init__(self):
        super().__init__()


class RasterScanDialog(CancelDiscardsChangesDialog):

    def __init__(self):
        super().__init__()


class MainWindow(QMainWindow, UiWidget):
    launch = pyqtSignal()  # A change has been made to the backend configuration and it must be completely reloaded. Also emitted on first launch
    scan_changed = pyqtSignal()  # Scan pattern has been changed. Passes frame sizes so backend can decide what needs to be done
    processing_changed = pyqtSignal(int, int)  # Processing parameters have been changed. Passes frame sizes so backend can decide what needs to be done
    closed = pyqtSignal()  # MainWindow has been closed
    scan = pyqtSignal()  # Scan command
    acquire = pyqtSignal()  # Acquire command
    stop = pyqtSignal()  # Stop command

    def __init__(self):
        super().__init__()

        # Populate layout with subwidgets
        replaceWidget(self.ScanGroupBox, ScanGroupBox())
        replaceWidget(self.DisplayWidget, DisplayWidget())
        replaceWidget(self.FileGroupBox, FileGroupBox())
        replaceWidget(self.SpectrumWidget, SpectrumWidget())
        replaceWidget(self.ProcessingGroupBox, ProcessingGroupBox())
        replaceWidget(self.ControlGroupBox, ControlGroupBox())
        self.ScanGroupBox = self.centralWidget.ScanGroupBox
        self.DisplayWidget = self.centralWidget.DisplayWidget
        self.FileGroupBox = self.centralWidget.FileGroupBox
        self.SpectrumWidget = self.centralWidget.SpectrumWidget
        self.ProcessingGroupBox = self.centralWidget.ProcessingGroupBox
        self.ControlGroupBox = self.centralWidget.ControlGroupBox

        self.statusBar().setSizeGripEnabled(False)
        self.setMaximumSize(self.minimumSize())

        self.ScanGroupBox.changed.connect(self._showRepeatProcessing)

        self._settings_dialog = SettingsDialog()
        self._settings_dialog.setParent(self)
        self._settings_dialog.setWindowFlag(True)

        self.actionSave_Configuration.triggered.connect(self.saveConfiguration)
        self.actionLoad_Configuration.triggered.connect(self.loadConfiguration)
        self.actionSettings.triggered.connect(self._settings_dialog.showDialog)
        self._settings_dialog.changed.connect(self.launch.emit)
        self._settings_dialog.checkNumberOfBuffersAuto.toggled.connect(
            lambda: self._settings_dialog.spinNumberOfBuffers.setEnabled(not self._settings_dialog.checkNumberOfBuffersAuto.isChecked())
        )

        self.ControlGroupBox.scan.connect(self._scan)
        self.ControlGroupBox.acquire.connect(self._acquire)
        self.ControlGroupBox.stop.connect(self._stop)

        # Emit frame sizes so we know if we need to change the buffer sizes
        self.ScanGroupBox.changed.connect(self.scan_changed.emit)
        self.ProcessingGroupBox.changed.connect(lambda: self.processing_changed.emit(self.unprocessed_frame_size(), self.processed_frame_size()))
        self._showRepeatProcessing()

    def print(self, msgl, msgr=""):
        self.statusBar.showMessage("{}                {}".format(msgl, msgr), 1000)

    def loadConfiguration(self, cfg_file=None):
        if cfg_file is None:
            cfg_file = QFileDialog.getOpenFileName(self, "Load Configuration File", OCTview.config_resource_location,
                                               "OCTview configuration file (*.ini)")[0]
        if os.path.exists(cfg_file):
            self.loadStateFromJson(cfg_file)
            self.launch.emit()

    def saveConfiguration(self):
        cfg_file = QFileDialog.getSaveFileName(self, "Save Configuration File", OCTview.config_resource_location,
                                           "OCTview configuration file (*.ini)")[0]
        if len(cfg_file) > 0:
            self.writeStateToJson(cfg_file)

    # Overload
    def closeEvent(self, event):
        quit_msg = "Are you sure you want to exit OCTview?"
        reply = QMessageBox.question(self, 'Message', quit_msg, QMessageBox.Yes, QMessageBox.No)
        if reply == QMessageBox.Yes:
            self.writeStateToJson(os.path.join(OCTview.config_resource_location, '.last.ini'))
            self.closed.emit()
            event.accept()
        else:
            event.ignore()

    def _scan(self):
        self.scan.emit()

    def _acquire(self):
        self.acquire.emit()

    def _stop(self):
        self.stop.emit()

    def set_mode_scanning(self):
        self.ControlGroupBox.set_mode_scanning()
        self.ScanGroupBox.toggleScanningMode(True)
        self.ProcessingGroupBox.toggleScanningMode(True)
        self.FileGroupBox.setEnabled(True)
        self.ScanGroupBox.setEnabled(True)
        self.ProcessingGroupBox.setEnabled(True)
        self.menubar.setEnabled(False)

    def set_mode_acquiring(self):
        self.ControlGroupBox.set_mode_acquiring()
        self.ScanGroupBox.toggleScanningMode(True)
        self.ProcessingGroupBox.toggleScanningMode(True)
        self.FileGroupBox.setEnabled(False)
        self.ScanGroupBox.setEnabled(False)
        self.ProcessingGroupBox.setEnabled(False)
        self.menubar.setEnabled(False)

    def set_mode_ready(self):
        self.ControlGroupBox.set_mode_ready()
        self.ScanGroupBox.toggleScanningMode(False)
        self.ProcessingGroupBox.toggleScanningMode(False)
        self.FileGroupBox.setEnabled(True)
        self.ScanGroupBox.setEnabled(True)
        self.ProcessingGroupBox.setEnabled(True)
        self.menubar.setEnabled(True)

    def set_mode_not_ready(self):
        self.ControlGroupBox.set_mode_not_ready()
        self.ScanGroupBox.toggleScanningMode(False)
        self.ProcessingGroupBox.toggleScanningMode(False)
        self.FileGroupBox.setEnabled(True)
        self.ScanGroupBox.setEnabled(True)
        self.ProcessingGroupBox.setEnabled(True)
        self.menubar.setEnabled(True)

    def _showRepeatProcessing(self):
        self.ProcessingGroupBox.setARepeatProcessingDisplay(self.ScanGroupBox.a_repeats())
        self.ProcessingGroupBox.setBRepeatProcessingDisplay(self.ScanGroupBox.b_repeats())

    # -- MainWindow's methods are backend's interface on entire GUI -------------------------------------------------

    def processing_enabled(self) -> bool:
        return self.ProcessingGroupBox.enabled()

    def background_subtraction_enabled(self) -> bool:
        return self.ProcessingGroupBox.background_subtraction()

    def interpolation_enabled(self) -> bool:
        return self.ProcessingGroupBox.interpolation()

    def interpdk(self) -> float:
        return self.ProcessingGroupBox.interpdk()

    def apodization_window(self) -> np.ndarray:
        return self.ProcessingGroupBox.apodization_window()(self.aline_size())

    def aline_repeat_processing(self) -> str:
        if self.scan_pattern().aline_repeat > 1:
            return self.ProcessingGroupBox.a_repeat_processing()
        else:
            return 0

    def bline_repeat_processing(self) -> str:
        if self.scan_pattern().bline_repeat > 1:
            return self.ProcessingGroupBox.b_repeat_processing()
        else:
            return 0

    def frame_averaging(self) -> int:
        return self.ProcessingGroupBox.frame_averaging()

    def uncropped_frame_size(self) -> int:
        return self.scan_pattern().points_in_scan

    def unprocessed_frame_size(self) -> int:
        return self.scan_pattern().points_in_image * self.aline_size()

    def processed_frame_size(self) -> int:
        try:
            processed_frame_size = self.roi_size() * self.scan_pattern().points_in_image
            if self.aline_repeat_processing() is not None:
                processed_frame_size = int(processed_frame_size / self.scan_pattern().aline_repeat)
            if self.bline_repeat_processing() is not None:
                processed_frame_size = int(processed_frame_size / self.scan_pattern().bline_repeat)
            return processed_frame_size
        except ZeroDivisionError:
            return 0

    def image_dimensions(self) -> (int, int, int):
        """The dimensions of the 3D image we expect to grab from the backend.

        TODO: verify (by asking backend) that this is the real size to avoid buffer overruns
        """
        pat = self.scan_pattern()
        n = pat.dimensions[0]
        if self.aline_repeat_processing() is None:
            n *= pat.aline_repeat
        aline_n = self.aline_size()
        if self.bline_repeat_processing() is None:
            n *= pat.bline_repeat
        return self.roi_size(), n, pat.dimensions[1]

    def scan_pattern(self) -> LineScanPattern:
        return self.ScanGroupBox.pattern()

    def darkTheme(self) -> bool:
        return self._settings_dialog.radioDark.isChecked()

    def camera_device_name(self) -> str:
        return self._settings_dialog.lineCameraName.text()

    def max_line_rate(self) -> int:
        return int(1000 * self._settings_dialog.spinMaxLineRate.value() - 1)  # Convert from kHz float to Hz int

    def zroi(self) -> (int, int):
        return self.ScanGroupBox.zroi()

    def roi_size(self) -> int:
        return self.zroi()[1] - self.zroi()[0]

    def roi_offset(self) -> int:
        return self.zroi()[0]

    def rotation_degrees(self) -> float:
        return self.ScanGroupBox.rotation_degrees()

    def scan_offset_mm_x(self) -> float:
        """Scan offset in mm"""
        return self.ScanGroupBox.x_offset()

    def scan_offset_mm_y(self) -> float:
        """Scan offset in mm"""
        return self.ScanGroupBox.y_offset()

    def aline_size(self) -> int:
        return self._settings_dialog.spinAlineSize.value()

    def frames_to_buffer(self) -> int:
        if self._settings_dialog.checkNumberOfBuffersAuto.isChecked():
            fsize = self.uncropped_frame_size()
            if fsize > 65536:
                b = 2
            elif fsize > 16384:
                b = 4
            elif fsize > 4096:
                b = 8
            elif fsize > 2048:
                b = 16
            else:
                b = 32
        else:
            b = int(self._settings_dialog.spinNumberOfBuffers.value())
        print('OCTview: Automatic buffer count calculator creating', b, 'buffers.')
        return b

    def analog_output_galvo_x_ch_name(self) -> str:
        return self._settings_dialog.lineXChName.text()

    def analog_output_galvo_y_ch_name(self) -> str:
        return self._settings_dialog.lineYChName.text()

    def analog_output_line_trig_ch_name(self) -> str:
        return self._settings_dialog.lineLineTrigChName.text()

    def analog_output_start_trig_ch_name(self) -> str:
        return self._settings_dialog.lineStartTrigChName.text()

    def frames_to_acquire(self) -> int:
        return self.ControlGroupBox.frames_to_acquire()

    def file_max_gb(self) -> float:
        return self.FileGroupBox.max_gb()

    def filename(self) -> str:
        return self.FileGroupBox.filename()

    def save_processed_data(self) -> bool:
        return self.FileGroupBox.save_processed()

    def display_frame(self, frame: np.ndarray):
        """Display a 3D frame using the display widgets."""
        fov = np.array(self.scan_pattern().fov) * 10**-3
        fov = np.concatenate([[self._settings_dialog.spinAxialPixelSize.value() * self.roi_size() * 10**-6], fov])
        self.DisplayWidget.display_frame(frame, fov=fov)

    def display_spectrum(self, spectrum: np.ndarray):
        self.SpectrumWidget.plot(np.linspace(*self.spectrometer_range(), self.aline_size()), spectrum)

    def trigger_gain(self) -> float:
        return self._settings_dialog.spinTriggerGain.value()
        return self._settings_dialog.spinTriggerGain.value()

    def scan_scale_factors(self) -> (float, float):
        """V/mm scale factors"""
        return (self._settings_dialog.spinXScaleFactor.value(),
                self._settings_dialog.spinYScaleFactor.value())

    def alines_per_buffer(self) -> int:
        """The number of A-lines per frame trigger pulse."""
        if self.blines_triggered():
            assert self.uncropped_frame_size() % self.scan_pattern().dimensions[1] == 0,\
                'Total A-lines triggered not evenly divisible by number of B-lines'
            return int(self.uncropped_frame_size() / self.scan_pattern().dimensions[1])
        else:
            return self.uncropped_frame_size()

    def max_line_rate(self) -> int:
        return int(self._settings_dialog.spinMaxLineRate.value())  # Hz

    def blines_triggered(self) -> bool:
        if self._settings_dialog.radioTriggerAuto.isChecked():
            return self.ScanGroupBox.ScanWidget.spinACount.value() * self.ScanGroupBox.ScanWidget.spinBCount.value() > OCTview.MAX_ALINES_IN_SINGLE_BUFFER
        else:
            return self._settings_dialog.radioTriggerBlines.isChecked()

    def dll_search_paths(self) -> list:
        return list(filter(lambda p: len(p) < 1, self._settings_dialog.textEditDLLPaths.toPlainText().split(r'\n')))

    def spectrometer_range(self) -> (int, int):
        return (self._settings_dialog.spinSpectrometerLow.value(), self._settings_dialog.spinSpectrometerHigh.value())

    def should_create_metadata_file(self) -> bool:
        return self.FileGroupBox.checkMetadata.isChecked()

    def axial_pixel_size_mm(self) -> float:
        return self._settings_dialog.spinAxialPixelSize.value() * 0.001
