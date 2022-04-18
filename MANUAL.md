# OCTview Manual v1.0.0

Open-source desktop application for spectral domain optical coherence tomography system control and image acquisition.

Available via GPL.

Authored by sstucker in 2022

Made possible by David Boas, Ph.D., Jianbo Tang, Ph.D, and the Neurophotonics Center at Boston University as well as NIH NeuroNex [Program #]

Dependent on the wonderful Qt and FFTW. Utilizes the IMAQ and DAQmx interfaces; this software is the property of NI (Austin, TX).

# Introduction

Spectral domain optical coherence tomography (SD-OCT) imaging systems are invaluable tools for opthamology, angiography, and even functional neuroimaging. Axial reflectance profiles, or A-lines, can be obtained from the Fourier transform of an interferometric measurement of a point on the sample in a process analogous to ultrasound. By scanning this point in two dimensions, a volumetric image can be acquired.

## Software

Because SD-OCT is fast, relatively non-invasive, and affords the user phase-resolved methods such as Doppler flow measurement, SD-OCT is an appealing platform for real-time functional imaging. However, the real-time processing of raw OCT interferograms is expensive given the high stream rate of the data. Commercially available line cameras can acquire A-lines in excess of 76,000 lines per second; even at this modest line rate, a 16 bit line camera with 2048 pixels producs 2.5 GB/s.

Each A-line must undergo processing before it can be interpreted spatially. This processing traditionally includes wavenumber-linearization interpolation, dispersion compensation, background subtraction/fixed-pattern noise removal, Fourier transform, and cropping to a subsection of the axial field of view. For vizualization, the maximum or mean value projection of the volumetric image is often computed along one or more axes. 

OCTview is based on a C++ library designed for a computer vision project that required real-time OCT volumes to be processed with minimal latency. By capitalizing on the highly parallelizable nature of A-line processing and the low-latency of multithreaded host-based computation, the OCTview backend code provides a real-time SD-OCT image stream.

The resulting application allows users to preview the results of popular processing routines such as those for angiography, improving the ease of system calibration, alignment, and use and ultimately image quality.

## Hardware

While optical designs vary based on the application, the complete SD-OCT system always integrates a spectrometer-coupled line camera and low coherence illumination source with a galvanometric scanning system. All these must be synchronized in their operation to yield a volumetric image. 

National Instruments provides a popular hardware platform for the multifunctional DAC and ADC devices required by an OCT system. OCTview employs NI's popular IMAQ and DAQmx software for control of scanners, triggering, and frame grabbing. OCTview can be configured to work with an array of NI hardware. 

In theory, the application's frontend can be attached to any hardware interface by reimplementing the backend interfaces. So far, only the NI backend exists.

## Why OCTview?
- *Powerful modular scan pattern designer.* OCTview uses PyScanPatterns to generate parametric scan patterns compatible with various scanners and imaging types.
- *High speed.* OCTview's processing routines are designed for execution at video rate.
- *Powerful, open code.* OCTview is not dependent on a sluggish, proprietary programming language like MATLAB or LabVIEW. OCTview can be developed using only the freely available versions of Python 3, Microsoft Visual C++, and Qt 5.
- *Acquire images out of the box.* The NI-based backend is compatible with hardware used by many imaging applications and shouldn't require any custom code.

# Installation & Setup

[TODO. Probably the fbs executable]

# Using OCTview

## Control

There are 3 buttons on the top right of the window which control the state of the imaging system:

- **Scan**: begin processing and displaying the images using the scan pattern and processing options specified. Does not stream any data to disk.
- **Acquire**: begin streaming images to disk as specified by the File settings.
- **Stop**: stop scanning/acquiring. Will interrupt acquisition if *n-frame acquisition* is underway.

## Scan pattern

Scan pattern

## Processing settings

Processing

## File settings

File settings

## GUI Configuration

The state of the GUI can be saved and loaded via File -> Save/Load Configuration. 

When OCTview is closed, the state of the GUI is written to `.last.oct` in the `<installation>/resources/windows/configuration` directory. This file is loaded automatically when OCTview is launched.

## Global settings

High-level application configuration is available via File -> Settings.