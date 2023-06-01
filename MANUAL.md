# OCTview Manual v1.0.0

Open-source desktop application for spectral domain optical coherence tomography system control and image acquisition.

Available via GPL.

Authored by sstucker in 2022

Made possible by David Boas, Ph.D., Jianbo Tang, Ph.D, and the Neurophotonics Center at Boston University.

Utilizes the IMAQ and DAQmx interfaces; this software is the property of NI (Austin, TX).

# Introduction

Spectral domain optical coherence tomography (SD-OCT) imaging systems are invaluable tools for opthamology, angiography, and even neuroimaging. Axial reflectance profiles, or A-lines, can be obtained from the Fourier transform of an interferometric measurement of a point on the sample in a process analogous to ultrasound. By scanning this point in two dimensions, a volumetric image can be acquired.

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

[Download](https://github.com/sstucker/OCTview/releases/latest) and run the latest OCTviewSetup.exe. Note that you may have to click <ins>More info</ins> to run the installer.

First time users should head to the File -> Global Settings menu to enter the NI-IMAQ and NI-DAQmx camera and channel names. The application will attempt to reload the hardware interface each time the Global Settings menu is closed with 'OK'. 

If you are having issues with any part of the application, run it from a command line utility to ensure you can view stdout. As of 7/2022 there is no logging in OCTview.

# Using OCTview

## Control

There are 3 buttons on the top right of the window which control the state of the imaging system:
![image](https://user-images.githubusercontent.com/22327925/180009286-d79c3bc3-47cb-4a9a-857a-dadce9fa6d08.png)

These buttons will be greyed out if the controller is not ready for the operation. This may occur if the backend is working on allocating new imaging buffers, planning processing, or uploading a large scan pattern.

- **Scan**: begin processing and displaying the images using the scan pattern and processing options specified. Does not stream any data to disk.
- **Acquire**: begin streaming images to disk as specified by the File settings.
- **Stop**: stop scanning/acquiring. Will interrupt acquisition if *n-frame acquisition* is underway.

**Aquire** can be clicked at any time. Note that there is no "pre-scanning" prior to image acquisition, so any distortions induced by the scanners as they jump to the new pattern will be present in the acquired data. 

Acquisitions can be infinite or numbered.

**Stop** will abort the acquisition at any time. The true number of frames acquired for an aborted acquisition is NOT recorded and must be determined from the frame dimensions.

## Scan pattern

![image](https://user-images.githubusercontent.com/22327925/180011489-7c780739-cdec-4e90-ae2c-4d7fcbabc299.png)
Scan patterns are generated from a number of parameters. These parameters are available to all scan patterns.
- **Z-top**: The number of voxels to crop from the top of the spatial A-line.
- **Z-bottom**: The number of voxels to crop from the bottom of the spatial A-line.
- **X/Y offset**: A lateral shift applied to the entire scan.
- **Rotation**: The rotation to apply to the scan. Note that rotated scans are demanding of both galvos and distort notions of fast-axis vs. slow-axis for raster patterns.

Currently, only raster scan patterns can be designed from the following parameters:
- **A-scan count**: Number of points to scan in the fast direction.
- **X-axis ROI width**: The width of the rectangular scan in physical units. See Global Settings to adjust the calibration factor for your scanner.
- **A-scan repeat**: The number of A-lines exposed at each point.
- **B-scan count**: The number of points to scan in the slow direction.
- **Y-axis ROI height**: The height of the rectangular scan in physical units.
- **Fast-axis/Slow-axis profile**: The profile the scanners take to interpolate the points of the scan. A stepped slow-axis is standard for slower scans.
- **Flyback duty**: The percentage of the imaging time to use for flyback. i.e. 0% uses zero samples for flyback and 100% uses equal samples for the scan and for flyback (the galvo signal will be triangular).
- **Exposure fraction**: The amount of the scan to use for imaging. This can be increased to mitigate galvo turning artifacts at the edges of the image. Note that for small scans which use relatively few samples this percentage is only an approximation.

## Processing settings
![image](https://user-images.githubusercontent.com/22327925/180014534-1f78b6cc-f7d2-4c9a-8f85-7b00b6e9c811.png)

Processed OCT data is always displayed. Several processing options exist:
- **Mean spectrum subtraction**: If checked, the average of the previous frame is subtracted from each A-line in the subsequent frame.
- **Lambda-k interpolation**: If checked, 1st order interpolation is performed in order to linearize the spectrum in wavenumber. Calibration of the delta-_k_ parameter can be performed easily by adjusting this value while inspecting the axial PSF of a single reflector at the focus.
- **Apodization**: If checked, the selected window function is multiplied by each spectrum prior to FFT.

FFT is always performed on spectra before their display.

## File settings

![image](https://user-images.githubusercontent.com/22327925/180015319-cf30181b-e798-4db0-8698-ab9ac87c868b.png)

OCTview allows for raw or processed data to be saved to disk in binary format. Data is saved in the order in which it was acquired: it can be considered a FORTRAN-ordered array with the dimensions [z, x, y, t]. Raw data is saved as UINT16 while processed data is saved as COMPLEX64.

Several parameters control the data acquisition:
- **Experiment directory**: The folder in which to save files.
- **Trial name**: The name of the current imaging trial. Depending on the volume of data acquired and the selected Maximum file size, multiple files with the suffix '\_0001', '\_0002' etc may be created.
- **Maximum file size**: The maximum number of bytes that will be written to a single file before creating a new one.
- **Save metadata .JSON**: If checked, a JSON file containing information about the acquisition will be acquired alongside the binary data.

## GUI Configuration
The state of the GUI can be saved and loaded via File -> Save/Load Configuration. 
When OCTview is closed, the state of the GUI is written to `.last.oct` in the `<installation>/resources/windows/configuration` directory. This file is loaded automatically when OCTview is launched.
