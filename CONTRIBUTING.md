## Recommended development environment
- Qt 5.9.2
- [Visual Studio 2017 Community Edition](https://visualstudio.microsoft.com/vs/older-downloads/), C++ Build Tools
   - The following libraries must be available to build the fastnisdoct library:
      - National Instruments [IMAQ](https://www.ni.com/en-us/support/downloads/drivers/download.vision-acquisition-software.html#409847) and [NI-DAQmx](https://www.ni.com/en-us/support/downloads/drivers/download.ni-daqmx.html#445931)
      - [FFTW](http://www.fftw.org/install/windows.html)
- [Python 3.6.8](https://www.python.org/downloads/release/python-368/) (install `requirements.txt`)

## Design principles
OCTview is generally organized into two parts: a backend dynamically-linked library written in C/C++ which interacts with hardware and performs parallelized image streaming and processing, and a Python-based application for scan-pattern generation and frontend interaction and display with Qt.

## Building OCTview with fbs

OCTview is a Windows desktop application built with Qt 5. 

A lightweight installer delivers OCTview to the user. To create this installer, prepare a Python 3.6.8 environment with the dependencies listed in `requirements.txt` and then run:
```
python -m fbs freeze
python -m fbs installer
```
The resulting installer will be in the `targets` directory.

This repository has an action that releases a new executable automatically when new version tags are pushed to the main branch.
