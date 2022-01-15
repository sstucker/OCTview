from PyQt5.QtWidgets import QWidget, QGridLayout, QGroupBox

import pyqtgraph as pyqtg
import os
from PyQt5 import uic


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
        self.__dict__[old_widget.objectName()] = new_widget  # Goodbye, old widget!


class CentralWidget(QWidget, UiWidget):

    def __init__(self):
        super(QWidget, self).__init__()
        super(UiWidget, self).__init__()

        self.replaceWidget(self.widgetSpec, pyqtg.ImageView())
        print(self.widgetSpec)

        self.show()