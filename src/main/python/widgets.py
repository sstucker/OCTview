import json
import os
import warnings

import numpy as np
from PyQt5 import uic
from PyQt5.QtCore import pyqtSignal, QObject, QThread, QTimer
from PyQt5.QtWidgets import QWidget, QLayout, QGridLayout, QGroupBox, QMainWindow, QSpinBox, QDoubleSpinBox, QCheckBox, QRadioButton, \
    QFileDialog, QMessageBox, QLineEdit, QTextEdit, QComboBox, QDialog, QFrame
from scanpatterns import LineScanPattern, RasterScanPattern

from pyqtgraph.graphicsItems.InfiniteLine import InfiniteLine as pyqtgraphSlider

import pyqtgraph
import pyqtgraph.opengl

from threading import Thread

import OCTview

ALINE_SIZE = 2048

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

    pattern_updated = pyqtSignal()

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

        self._settings_dialog = RasterScanDialog()
        self._settings_dialog.setParent(self)
        self._settings_dialog.setWindowFlag(True)

        self.buttonSettings.pressed.connect(self._settings_dialog.showDialog)

        # Pattern generation takes place in this thread
        self._pattern_gen_thread = Thread()
        self.pattern_updated.connect(self._pattern_generated)
        # _pattern_generated assigns _new_pattern to _pattern
        self._new_pattern = RasterScanPattern()
        self._pattern = RasterScanPattern()

    def _generate_pattern(self, **kwargs):
        """Creates new scan pattern and assigns it to _new_pattern and then emits 'pattern_updated'.

        _new_pattern should be created here and read in the main thread in the callback ONLY..

        Args:
            **kwargs: arguments passed to `RasterScanPattern.generate()`
        """
        self._new_pattern = RasterScanPattern()
        self._new_pattern.generate(**kwargs)
        self.pattern_updated.emit()

    def _pattern_generated(self):
        self._pattern = self._new_pattern  # New pattern should not be read anywhere else
        self._pattern_gen_thread.join()
        self.parentWidget().linePatternRate.setText(str(self._pattern.pattern_rate)[0:5] + ' Hz')

    def generate_pattern(self):
        if self._pattern_gen_thread.is_alive():
            """
            Don't start new pattern gen if there is already some in progress. This keeps GUI responsive with the expense
            that quick successive changes to a large pattern may not be reflected in the GUI before scanning
            """
            return
        self._pattern_gen_thread = Thread(target=self._generate_pattern, kwargs={
            'alines': self.spinACount.value(),
            'blines': self.spinBCount.value(),
            'max_trigger_rate': 76000,
            'fov': [self.spinROIWidth.value(), self.spinROIHeight.value()],
            'flyback_duty': self.spinFlybackDuty.value() / 100,
            'exposure_fraction': self.spinExposureFraction.value() / 100,
            'fast_axis_step': self.radioXStep.isChecked(),
            'slow_axis_step': self.radioYStep.isChecked(),
            'aline_repeat': [1, self.spinARepeat.value()][int(self.checkARepeat.isChecked())],
            'bline_repeat': [1, self.spinBRepeat.value()][int(self.checkBRepeat.isChecked())],
            'bidirectional': self.checkBidirectional.isChecked(),
            'rotation_rad': self.parentWidget().spinRotation.value() * np.pi / 180,
        })
        self._pattern_gen_thread.start()

    def fixSize(self, fixed: bool):
        """Disables all widgets that allow the editing of the image size. This cannot be changed during a scan.

        Args:
            fixed (bool): if True, widgets are disabled.
        """
        self.spinARepeat.setEnabled(not fixed)
        self.spinBRepeat.setEnabled(not fixed)
        self.checkARepeat.setEnabled(not fixed)
        self.checkBRepeat.setEnabled(not fixed)
        self.spinACount.setEnabled(not fixed)
        self.spinBCount.setEnabled(not fixed)
        self.checkSquareScan.setEnabled(not fixed)
        self.buttonSettings.setEnabled(not fixed)

    @property
    def a_repeats(self):
        if self.checkARepeat.isChecked():
            return self.spinARepeat.value()
        else:
            return 0

    @property
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
        self.spinExposureFraction.setEnabled(not self.radioXStep.isChecked())
        self.labelExposureFraction.setEnabled(not self.radioXStep.isChecked())
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

    changed = pyqtSignal()

    def __init__(self):
        super().__init__()

        self._subwidgets = {
            'Raster': RasterScanWidget(),
            'Line': LineScanWidget(),
            'Circle': CircleScanWidget()
        }
        self._selectSubwidget()
        self.comboScanPattern.currentIndexChanged.connect(self._selectSubwidget)

        # All widgets should emit the scan pattern changed signal
        children = self.children()
        for subwidget in list(self._subwidgets.values()):
            children += subwidget.children()
        for child in children:
            if type(child) in [QSpinBox, QDoubleSpinBox]:
                child.valueChanged.connect(self.changed.emit)
            elif type(child) in [QCheckBox]:
                child.stateChanged.connect(self.changed.emit)
            elif type(child) in [QRadioButton]:
                child.toggled.connect(self.changed.emit)

        self.changed.connect(self.ScanWidget.generate_pattern)

        self.pushPreview.pressed.connect(self._preview)

        self.changed.emit()

    def _selectSubwidget(self):
        replaceWidget(self.ScanWidget, self._subwidgets[self.comboScanPattern.currentText()])
        self.ScanWidget.generate_pattern()

    def _preview(self):
        print('Scan pattern preview')
        import matplotlib.pyplot as plt
        plt.plot(self.x)
        plt.plot(self.y)
        plt.plot(self.line_trigger)
        plt.show()

    def toggleScanningMode(self, scanning_mode: bool):
        self.ScanWidget.fixSize(scanning_mode)
        self.spinZTop.setEnabled(not scanning_mode)
        self.labelZTop.setEnabled(not scanning_mode)
        self.spinZBottom.setEnabled(not scanning_mode)
        self.labelZBottom.setEnabled(not scanning_mode)
        self.comboScanPattern.setEnabled(not scanning_mode)
        self.labelScanPatternType.setEnabled(not scanning_mode)

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

    @property
    def a_repeats(self):
        return self.ScanWidget.a_repeats

    @property
    def b_repeats(self):
        return self.ScanWidget.b_repeats


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

    @property
    def save_metadata(self):
        return self.checkMetadata.isChecked()

    @property
    def filetype(self):
        return self.comboFileType.currentIndex()


class ProcessingGroupBox(QGroupBox, UiWidget):

    def __init__(self):
        super().__init__()

        self._windows = {
            'Hanning': np.hanning(ALINE_SIZE),
            'Blackman': np.blackman(ALINE_SIZE)
        }
        self._null_window = np.ones(ALINE_SIZE)

        self.checkApodization.toggled.connect(self._apodToggled)
        self.checkInterpolation.toggled.connect(self._interpToggled)
        self.radioFrameNone.toggled.connect(self._frameProcessingToggled)

    def _apodToggled(self):
        self.labelApodization.setEnabled(self.checkApodization.isChecked())
        self.comboApodization.setEnabled(self.checkApodization.isChecked())

    def _interpToggled(self):
        self.labelInterpDk.setEnabled(self.checkInterpolation.isChecked())
        self.spinInterpDk.setEnabled(self.checkInterpolation.isChecked())

    def _frameProcessingToggled(self):
        self.spinFrameAverage.setEnabled(self.radioFrameAverage.isChecked())

    def setRepeatProcessingDisplay(self, shown: bool):
        self.groupRepeatProcessing.setVisible(shown)

    def toggleScanningMode(self, scanning: bool):
        self.groupFrameProcessing.setEnabled(not scanning)
        self.groupRepeatProcessing.setEnabled(not scanning)
        self.setCheckable(not scanning)

    @property
    def apodization_window(self):
        if self.checkApodization.isChecked() and self.isChecked():
            return self._windows[self.comboApodization.currentText()]
        else:
            return self._null_window

    @property
    def interpolation(self):
        if self.isChecked():
            return self.checkInterpolation.isChecked(), self.spinInterpDk.value()
        else:
            return False

    @property
    def background_subtraction(self):
        if self.isChecked():
            return self.checkMeanSpectrumSubtraction.isChecked()
        else:
            return False


class SpectrumPlotWidget(pyqtgraph.GraphicsWindow):

    yrange = property()
    wavelengths = property()

    def __init__(self, wavelengths, yrange=[0, 4096]):

        super().__init__()

        self.resize(200, 100)

        self._wavelengths = wavelengths
        self._n = len(self._wavelengths)

        self._plot = self.addPlot()
        self._plot.buttonsHidden = True
        self._plot.setYRange(yrange[0], yrange[1])
        self._plot.setLabel('bottom', text='λ (nm)')

        self._spectrum = self._plot.plot(color='#FFFFFF')

        self._current_data = 0
        self.plot(np.zeros(self._n))

    def plot(self, data):
        self._current_data = data
        self._spectrum.setData(self._wavelengths, self._current_data)
        pyqtgraph.QtGui.QApplication.processEvents()

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

        img = np.random.random([128, 128, 200])
        data = np.empty(img.shape + (4,), dtype=np.ubyte)
        data[..., 0] = img * (255. / (img.max() / 1))
        data[..., 3] = (data[..., 3].astype(float) / 255.) ** 2 * 255

        self._volume = pyqtgraph.opengl.GLVolumeItem(data, sliceDensity=1, smooth=True, glOptions='translucent')

        self.opts['distance'] = 600
        # self.addItem(self._grid)
        self.addItem(self._axis)
        self.addItem(self._volume)
        self.show()

    def updateData(self, volume):

        extent = volume.shape * 3
        # self._grid.setSize(x=extent[0], y=extent[1], z=extent[2])
        self._axis.setSize(x=extent[0], y=extent[1], z=extent[2])

        data = np.empty(volume.shape + (4,), dtype=np.ubyte)
        data[..., 0] = volume * (255. / (volume.max() / 1))
        data[..., 3] = (data[..., 3].astype(float) / 255.) ** 2 * 255
        self._volume.setData(data)


class BScanWidget(pyqtgraph.GraphicsLayoutWidget):

    def __init__(self, title=None, vert_slider=True):
        super().__init__()

        self._plot = self.addPlot()
        self._plot.setAspectLocked(True)
        self._plot.showGrid(x=True, y=True)
        self._plot.invertY()
        self._plot.setLabel('left', units='m')
        self._plot.setLabel('bottom', units='m')
        self._image = pyqtgraph.ImageItem()
        self._plot.addItem(self._image)
        if vert_slider:
            self.slider_angle = 0
        else:
            self.slider_angle = 90
        self._slider = pyqtgraphSlider(movable=True, angle=self.slider_angle)
        self._slider.setPen(width=4, color='y')
        self._slider.setHoverPen(width=6, color='#FFFFFF')
        self._slider.sigPositionChanged.connect(self._setSlice)
        self._slider.setZValue(1)
        self._plot.addItem(self._slider)

        self._index = 0
        self._sf = [1, 1]
        self._data_shape = None
        # if title is not None:
        #     self._plot.setLabel('top', text=title)

    def setAspect(self, dx, dy):
        self._image.scale(1 / self._sf[0], 1 / self._sf[1])  # Undo any previous scaling
        sfx = (1 / self._data_shape[0]) * dx
        sfy = (1 / self._data_shape[1]) * dy
        self._sf = [sfx, sfy]
        if self.slider_angle == 90:
            fov = self._sf[0] * self._data_shape[0]
        else:
            fov = self._sf[1] * self._data_shape[1]
        self._slider.setBounds([0, fov])
        self._image.scale(sfx, sfy)

    def updateData(self, volume, fov=None):
        self._data_shape = volume.shape
        if fov is not None:
            self.setAspect(fov[0], fov[1])
        self._image.setImage(volume[..., int(self._slider.value())])

    def _setSlice(self):
        if self.slider_angle == 90:
            fov = self._sf[0]
        else:
            fov = self._sf[1]
        self._slice = int(self._slider.value() / fov)

    def showSlider(self, show: bool):
        if show:
            self._slider.show()
        else:
            self._slider.hide()


class DisplayWidget(QWidget, UiWidget):

    def __init__(self):
        super().__init__()

        self._volume = VolumeWidget()
        self._enface = BScanWidget(vert_slider=False, title='Enface view')
        self._bscan = BScanWidget(title='B-scan view')

        replaceWidget(self.VolumeWidget, self._volume)
        replaceWidget(self.BScanWidget, self._enface)
        replaceWidget(self.EnfaceWidget, self._bscan)

        self._timer = QTimer()
        self._timer.timeout.connect(self._update)
        self._timer.start(100)

    def _update(self):
        img = np.random.random([128, 128, 200])
        if self.tabDisplay.currentIndex() == 0:
            self._enface.updateData(img, fov=[1 * 10**-6, 1 * 10**-6])
            self._bscan.updateData(img, fov=[1 * 10**-6, 1 * 10**-6])
        elif self.tabDisplay.currentIndex() == 1:
            self._volume.updateData(img)
        pyqtgraph.QtGui.QApplication.processEvents()


class SpectrumWidget(QWidget, UiWidget):

    def __init__(self):
        super().__init__()
        replaceWidget(self.SpectrumPlotWidget, SpectrumPlotWidget(np.arange(0, 2048)))


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

    reload_required = pyqtSignal()

    def __init__(self):
        super().__init__()

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
        # self.setFixedSize(self.minimumSize())

        self.ScanGroupBox.changed.connect(
            lambda: self.ProcessingGroupBox.setRepeatProcessingDisplay(
                self.ScanGroupBox.a_repeats > 1
                or self.ScanGroupBox.b_repeats > 1
            )
        )

        self._settings_dialog = SettingsDialog()
        self._settings_dialog.setParent(self)
        self._settings_dialog.setWindowFlag(True)

        self.actionSave_Configuration.triggered.connect(self.saveConfiguration)
        self.actionLoad_Configuration.triggered.connect(self.loadConfiguration)
        self.actionSettings.triggered.connect(self._settings_dialog.showDialog)
        self._settings_dialog.changed.connect(self.reload_required.emit)

        config_file = os.path.join(OCTview.config_resource_location, '.last')
        if os.path.exists(config_file):
            self.loadStateFromJson(config_file)

        self.ControlGroupBox.scan.connect(lambda: self.toggleScanningMode(True))
        self.ControlGroupBox.acquire.connect(lambda: self.toggleScanningMode(False))

        self.show()


    def loadConfiguration(self):
        file = QFileDialog.getOpenFileName(self, "Load Configuration File", OCTview.config_resource_location, "OCTview configuration file (*.oct)")[0]
        if os.path.exists(file):
            self.loadStateFromJson(file)

    def saveConfiguration(self):
        file = QFileDialog.getSaveFileName(self, "Save Configuration File", OCTview.config_resource_location, "OCTview configuration file (*.oct)")[0]
        if len(file) > 0:
            self.writeStateToJson(file)

    # Overload
    def closeEvent(self, event):
        quit_msg = "Are you sure you want to exit OCTview?"
        reply = QMessageBox.question(self, 'Message', quit_msg, QMessageBox.Yes, QMessageBox.No)
        if reply == QMessageBox.Yes:
            self.writeStateToJson(os.path.join(OCTview.config_resource_location, '.last'))
            event.accept()
        else:
            event.ignore()

    def toggleScanningMode(self, scanning: bool):
        self.ScanGroupBox.toggleScanningMode(scanning)
        self.ProcessingGroupBox.toggleScanningMode(scanning)

    # -- MainWindow's properties are backend's interface on entire GUI -------------------------------------------------

    @property
    def darkTheme(self):
        return self._settings_dialog.radioDark.isChecked()


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
