## Recommended development environment
- Qt 5.9.2
- [Visual Studio 2017 Community Edition](https://visualstudio.microsoft.com/vs/older-downloads/), C++ Build Tools
   - The following libraries must be available to build the fastnisdoct library:
      - National Instruments [IMAQ](https://www.ni.com/en-us/support/downloads/drivers/download.vision-acquisition-software.html#409847) and [NI-DAQmx](https://www.ni.com/en-us/support/downloads/drivers/download.ni-daqmx.html#445931)
      - [FFTW](http://www.fftw.org/install/windows.html)
- [Python 3.6.8](https://www.python.org/downloads/release/python-368/) (install `requirements.txt`)

## Design
OCTview is generally organized into two parts: a backend dynamically-linked library written in C/C++ which interacts with hardware and performs parallelized image streaming and processing, and a Python-based application for scan-pattern generation and frontend interaction and display with Qt.

### Front-end
The `MainWindow` class (widgets.py) exposes the various `Widget`s states and outputs to the `ApplicationContext` (OCTview.py), whose signals and slots dictate the main business of the application. The `ApplicationContext` interacts with the backend via calls to the controller (controller.py) which is a Python wrapper around the fastnisdoct DLL. 

### Back-end
Functions in the `extern "C"` block at the end of fastnisdoct.cpp are exposed to the Python environment.
These functions send messages into the main thread. The main thread is responsible for preparing the application for scanning or acquisition and updating it with the contents of the latest messages. This largely takes place in the `recv_msg` function which is called at the beginning of each main thread loop.

An `AlineProcesingPool` manages asynchronous processing of spectral A-lines into spatial A-lines.
A `FileStreamWorker` manages asynchronous streaming of image frames to disk.

`CircAcqBuffer` instances are ring buffers which connect the asynchronous workers. Frames are copied into the head of a ring buffer by the producer (the main thread) and can then be "locked out" by a consumer. (the file stream worker)

## Building OCTview with fbs

OCTview is a Windows desktop application built with Qt 5. 

A lightweight installer delivers OCTview to the user. To create this installer, prepare a Python 3.6.8 environment with the dependencies listed in `requirements.txt` and then run:
```
python -m fbs freeze
python -m fbs installer
```
The resulting installer will be in the `targets` directory.

This repository has an action that releases a new executable automatically when new version tags are pushed to the main branch.

This would be possible without the awesome https://build-system.fman.io/!
