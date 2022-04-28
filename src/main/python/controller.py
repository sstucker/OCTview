import ctypes as c
import numpy as np
from numpy.ctypeslib import ndpointer
import os

c_bool_p = ndpointer(dtype=np.bool, ndim=1, flags='C_CONTIGUOUS')
c_int_p = ndpointer(dtype=np.int32, ndim=1, flags='C_CONTIGUOUS')
c_bool_p = ndpointer(dtype=np.bool, ndim=1, flags='C_CONTIGUOUS')
c_uint16_p = ndpointer(dtype=np.uint16, ndim=1, flags='C_CONTIGUOUS')
c_uint32_p = ndpointer(dtype=np.uint32, ndim=1, flags='C_CONTIGUOUS')
c_float_p = ndpointer(dtype=np.float32, ndim=1, flags='C_CONTIGUOUS')
c_double_p = ndpointer(dtype=np.float64, ndim=1, flags='C_CONTIGUOUS')
c_complex64_p = ndpointer(dtype=np.complex64, ndim=1, flags='C_CONTIGUOUS')
c_complex64_p_3d = ndpointer(dtype=np.complex64, ndim=3, flags='C_CONTIGUOUS')


class NIOCTController:
    """
    In principle, it is possible to attach a new imaging system backend to the OCTview GUI by reimplementing NIOCTController.
    """

    def __init__(self, library):
        """Interface to imaging system hardware using NI-IMAQ and NI-DAQ libraries via a ctypes-wrapped .dll.
        
        Args:
            library (str): Path to the NIOCTController .dll
        """
        print('Loading backend .dll from', library)
        self._lib = c.CDLL(library)
        self._lib.nisdoct_open.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p, c.c_char_p, c.c_char_p, c.c_char_p,
                                           c.c_int]
        self._lib.nisdoct_configure_image.argtypes = [c.c_int, c.c_long, c_bool_p, c.c_long, c.c_long, c.c_long,
                                                      c.c_int, c.c_int, c.c_int, c.c_int, c.c_int, c.c_int, c_double_p,
                                                      c_double_p, c_double_p, c_double_p, c.c_long, c.c_int]
        self._lib.nisdoct_configure_processing.argtypes = [c.c_bool, c.c_bool, c.c_double, c_float_p, c.c_int, c.c_int]
        self._lib.nisdoct_start_raw_acquisition.argtypes = [c.c_char_p, c.c_longlong, c.c_int]
        self._lib.nisdoct_grab_frame.argtypes = [c_complex64_p]
        self._lib.nisdoct_grab_spectrum.argtypes = [c_float_p]

        self._lib.nisdoct_get_state.restype = c.c_int
        self._lib.nisdoct_ready.restype = c.c_bool
        self._lib.nisdoct_scanning.restype = c.c_bool
        self._lib.nisdoct_acquiring.restype = c.c_bool

    def open(self,
             camera_name,
             ao_ch_x_name,
             ao_ch_y_name,
             ao_ch_lt_name,
             ao_ch_ft_name,
             ao_ch_st_name,
             number_of_buffers
             ):
        """Open the interface. To change these values, the interface must be closed and then opened again.

        Args:
            camera_name (str): Name of cam file corresponding to line camera
            ao_ch_x_name (str): NI-DAQ analog out channel identifier to be used for X galvo output
            ao_ch_y_name (str): NI-DAQ analog out channel identifier to be used for Y galvo output
            ao_ch_lt_name (str): NI-DAQ analog out channel identifier to be used for camera triggering
            ao_ch_ft_name (str): NI-DAQ analog out channel identifier to be used for frame grab triggering
            ao_ch_st_name (str): NI-DAQ analog out channel identifier to be used to trigger other devices on imaging start
            number_of_buffers (int): The number of buffers to allocate for image acquisition and processing. Larger
                values make acquisition more robust to dropped frames but increase memory overhead.
        """
        self._lib.nisdoct_open(
            bytes(camera_name, encoding='utf8'),
            bytes(ao_ch_x_name, encoding='utf8'),
            bytes(ao_ch_y_name, encoding='utf8'),
            bytes(ao_ch_lt_name, encoding='utf8'),
            bytes(ao_ch_ft_name, encoding='utf8'),
            bytes(ao_ch_st_name, encoding='utf8'),
            int(number_of_buffers)
        )

    def close(self):
        self._lib.nisdoct_close()

    def configure_image(
            self,
            aline_size: int,
            alines_in_scan: np.long,
            image_mask: np.ndarray,
            alines_in_image: int,
            alines_per_bline: int,
            alines_per_buffer: int,
            x_scan_signal: np.ndarray,
            y_scan_signal: np.ndarray,
            line_trigger_scan_signal: np.ndarray,
            frame_trigger_scan_signal: np.ndarray,
            signal_output_rate: int,
            line_rate: int,
            aline_repeat: int,
            bline_repeat: int,
            roi_offset=0,
            roi_size=None,
            aline_repeat_processing: str = None,
            bline_repeat_processing: str = None,
    ):
        """Configures attributes of OCT acquisition such as image size.

        Acquisition cannot be configured during a scan.

        Args:
            aline_size (int): The number of voxels in each A-line i.e. 2048
            alines_in_scan (int): The total A-lines exposed during each acquisition frame.
            image_mask (np.ndarray): Boolean array of length `alines_in_scan` which is True for A-lines which are to be
                copied to the acquisition frame.
            alines_in_image (int): The total A-lines copied to the image.
            alines_per_bline (int): Size of each B-line subdivision of the acquisition frame.
            alines_per_buffer (int): Each grab buffer will be sized to contain this many A-lines.
            aline_repeat (int): if > 1, number of repeated successive A-lines in the scan. Defined by the scan pattern.
            bline_repeat (int): if > 1, number of repeated successive B-lines in the scan. Defined by the scan pattern.
            aline_repeat_processing (str): Defines processing to be carried out on repeated A-lines. Can be None or
                `'average'`. If `aline_repeat` is 2, can be `'difference'`. Default None.
            bline_repeat_processing (str): Defines processing to be carried out on repeated B-lines. Can be None or
                `'average'`. If `aline_repeat` is 2, can be `'difference'`. Default None.
            roi_offset (optional int): Number of voxels to discard from beginning of each spatial A-line
            roi_size (optional int): Number of voxels to keep of each spatial A-line, beginning from roi_offset
            x_scan_signal (np.ndarray): X galvo drive signal.
            y_scan_signal (np.ndarray): Y galvo drive signal.
            line_trigger_scan_signal (np.ndarray): Camera A-line exposure trigger signal.
            frame_trigger_scan_signal (np.ndarray): Frame grabber trigger signal.
            signal_output_rate (int): Sample generation rate.
            line_rate (int): Line rate. Should be evenly divisible into `signal_output_rate`.
        """
        a_rpt_proc_flag = 0
        b_rpt_proc_flag = 0
        for rpt_proc, flag in zip((aline_repeat_processing, bline_repeat_processing),
                                  (a_rpt_proc_flag, a_rpt_proc_flag)):
            if rpt_proc == 'average' or rpt_proc == 1:
                flag = 1
            elif rpt_proc == 'difference' or rpt_proc == 2:
                flag = 2
        if roi_size is None:
            roi_size = aline_size
        self._lib.nisdoct_configure_image(
            int(aline_size),
            np.long(alines_in_scan),
            np.array(image_mask).astype(bool),
            np.long(alines_in_image),
            np.long(alines_per_bline),
            np.long(alines_per_buffer),
            int(aline_repeat),
            int(bline_repeat),
            int(a_rpt_proc_flag),
            int(b_rpt_proc_flag),
            int(roi_offset),
            int(roi_size),
            np.array(x_scan_signal).astype(np.float64),
            np.array(y_scan_signal).astype(np.float64),
            np.array(line_trigger_scan_signal).astype(np.float64),
            np.array(frame_trigger_scan_signal).astype(np.float64),
            np.long(len(x_scan_signal)),
            np.long(signal_output_rate),
            int(line_rate)
        )

    def configure_processing(
            self,
            subtract_background: bool,
            interp: bool,
            intpdk: float,
            apod_window: np.ndarray,
            n_frame_avg: int = 0
    ):
        """Set parameters of SD-OCT processing. Can be called during a scan.
        Args:
            subtract_background (bool): If True, carry out background subtraction on each A-line.
            interp (bool): If True, carry out linear-in-wavelength -> linear-in-wavenumber interpolation.
            intpdk (float): Parameter for linear-in-wavelength -> linear-in-wavenmber interpolation.
            apod_window (np.ndarray): Window which is multiplied by each spectral A-line prior to FFT i.e. Hanning window.
            n_frame_avg (int): if > 1, frames to average together. Frame size is defined by `configure_image`. Default 0.
        """
        self._lib.nisdoct_configure_processing(
            bool(subtract_background),
            bool(interp),
            float(intpdk),
            np.array(apod_window).astype(np.float32),
            len(apod_window),  # aline_size
            int(n_frame_avg)
        )

    def start_scan(self):
        """Starts scanning if system is ready."""
        self._lib.nisdoct_start_scan()

    def stop_scan(self):
        """Stops scanning."""
        self._lib.nisdoct_stop_scan()

    def start_acquisition(self, file: str, max_bytes: np.longlong, frames_to_acquire: int = -1):
        """Starts streaming arrays to disk at the path supplied by file.

        Only successful if the controller is scanning.

        Args:
            file: Path on disk.
            max_bytes: Maximum size of each file before starting a new one.
            frames_to_acquire: The number of frames to acquire. If -1, acquisition continues until `stop_acquisition` is called.
        """
        self._lib.nisdoct_start_raw_acquisition(
            bytes(file, encoding='utf8'),
            np.longlong(max_bytes),
            int(frames_to_acquire)
        )

    def stop_acquisition(self):
        """Stops acquisition of a file. Will interrupt a numbered acquisition."""
        self._lib.nisdoct_stop_acquisition()

    def get_state(self) -> int:
        """Get state of the controller as an integer.

        Returns: 1 if UNOPENED, 2 if OPEN 3 if READY, 4 if SCANNING, 5 if ACQUIRING, 6 if ERROR.
        """
        return self._lib.nisdoct_get_state()

    @property
    def state(self) -> str:
        """Returns string form of state."""
        return ['', 'unopened', 'open', 'ready', 'scanning', 'acquiring', 'error'][self.get_state()]

    def ready(self) -> bool:
        """Returns True if controller is in the READY state."""
        return self._lib.nisdoct_ready()

    def scanning(self) -> bool:
        """Returns True if controller is in the SCANNING state."""
        return self._lib.nisdoct_scanning()

    def acquiring(self) -> bool:
        """Returns True if controller is in the ACQUIRING state."""
        return self._lib.nisdoct_acquiring()

    def grab_frame(self, output):
        return self._lib.nisdoct_grab_frame(output)

    def grab_spectrum(self, output):
        return self._lib.nisdoct_grab_spectrum(output)
