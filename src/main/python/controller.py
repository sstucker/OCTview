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
        self._lib.nisdoct_open.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p, c.c_char_p, c.c_char_p, c.c_char_p]
        self._lib.nisdoct_configure_image.argtypes = [c.c_int, c.c_int, c.c_int, c.c_int, c.c_int, c.c_int, c.c_int, c.c_int, c.c_int]
        self._lib.nisdoct_configure_processing.argtypes = [c.c_bool, c.c_bool, c.c_bool, c.c_double, c_float_p, c.c_int, c.c_int, c.c_int]
        self._lib.nisdoct_set_pattern.argtypes = [c_double_p, c_double_p, c_double_p, c_double_p, c.c_int]
        self._lib.nisdoct_start_acquisition.argtypes = [c.c_char_p, c.c_longlong, c.c_int]
        self._lib.nisdoct_grab_frame.argtypes = [c_complex64_p_3d]

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
             ):
        """Open the interface. To change these values, the interface must be closed and then opened again.

        Args:
            camera_name (str): Name of cam file corresponding to line camera
            ao_ch_x_name (str): NI-DAQ analog out channel identifier to be used for X galvo output
            ao_ch_y_name (str): NI-DAQ analog out channel identifier to be used for Y galvo output
            ao_ch_lt_name (str): NI-DAQ analog out channel identifier to be used for camera triggering
            ao_ch_ft_name (str): NI-DAQ analog out channel identifier to be used for frame grab triggering
            ao_ch_st_name (str): NI-DAQ analog out channel identifier to be used to trigger other devices on imaging start
        """

        # Open the interface
        # self._camera_name = bytes(camera_name, encoding='utf8')
        # self._ao_ch_x_name = bytes(ao_ch_x_name, encoding='utf8')
        # self._ao_ch_y_name = bytes(ao_ch_y_name, encoding='utf8')
        # self._ao_ch_lt_name = bytes(ao_ch_lt_name, encoding='utf8')
        # self._ao_ch_ft_name = bytes(ao_ch_ft_name, encoding='utf8')
        # self._ao_ch_st_name = bytes(ao_ch_st_name, encoding='utf8')

        self._lib.nisdoct_open(
            bytes(camera_name, encoding='utf8'),
            bytes(ao_ch_x_name, encoding='utf8'),
            bytes(ao_ch_y_name, encoding='utf8'),
            bytes(ao_ch_lt_name, encoding='utf8'),
            bytes(ao_ch_ft_name, encoding='utf8'),
            bytes(ao_ch_st_name, encoding='utf8')
        )

    def close(self):
        self._lib.nisdoct_close()

    def configure_image(
            self,
            dac_output_rate: int,
            aline_size: int,
            number_of_alines: int,
            alines_per_b: int,
            aline_repeat: int,
            bline_repeat: int,
            number_of_buffers: int,
            roi_offset=0,
            roi_size=None
        ):
        """Configures attributes of OCT acquisition such as image size.

        Acquisition cannot be configured during a scan.

        Args:
            dac_output_rate (int): The rate at which the DAC generates the samples passed to set_scan.
            aline_size (int): The number of voxels in each A-line i.e. 2048
            number_of_alines (int): The number of A-lines in each acquisition frame. Defined by the scan pattern.
            alines_per_b (int): Size of each B-line in A-lines. B-lines are the subdivisions of the acquisition frame. Defined by the scan pattern.
            aline_repeat (int): if > 1, number of repeated successive A-lines in the scan. Defined by the scan pattern.
            bline_repeat (int): if > 1, number of repeated successive B-lines in the scan. Defined by the scan pattern.
            number_of_buffers (int): The number of buffers to allocate for image acquisition and processing. Larger
                                      values make acquisition more robust to dropped frames but increase memory overhead.
            roi_offset (optional int): Number of voxels to discard from beginning of each spatial A-line
            roi_size (optional int): Number of voxels to keep of each spatial A-line, beginning from roi_offset
        """
        if roi_size is None:
            roi_size = aline_size
        self._lib.nisdoct_configure_image(
            int(dac_output_rate),
            int(aline_size),
            int(number_of_alines),
            int(alines_per_b),
            int(aline_repeat),
            int(bline_repeat),
            int(number_of_buffers),
            int(roi_offset),
            int(roi_size),
        )

    def configure_processing(
            self,
            enabled: bool,
            subtract_background: bool,
            interp: bool,
            intpdk: float,
            apod_window: np.ndarray,
            aline_repeat_processing: str = None,
            bline_repeat_processing: str = None,
            n_frame_avg: int = 0
    ):
        """Set parameters of SD-OCT processing. Can be called during a scan.
        Args:
            enabled (bool): If False, spectral data is saved and the rest of the configuration is ignored.
            subtract_background (bool): If True, carry out background subtraction on each A-line.
            interp (bool): If True, carry out linear-in-wavelength -> linear-in-wavenumber interpolation.
            intpdk (float): Parameter for linear-in-wavelength -> linear-in-wavenmber interpolation.
            apod_window (np.ndarray): Window which is multiplied by each spectral A-line prior to FFT i.e. Hanning window.
            aline_repeat_processing (str): Defines processing to be carried out on repeated A-lines. Can be None or `'average'`. If `aline_repeat` is 2, can be `'difference'`. Default None.
            bline_repeat_processing (str): Defines processing to be carried out on repeated B-lines. Can be None or `'average'`. If `aline_repeat` is 2, can be `'difference'`. Default None.
            n_frame_avg (int): if > 1, frames to average together. Frame size is defined by `configure_image`. Default 0.
        """
        a_rpt_proc_flag = 0
        b_rpt_proc_flag = 0
        for rpt_proc, flag in zip((aline_repeat_processing, bline_repeat_processing), (a_rpt_proc_flag, a_rpt_proc_flag)):
            if rpt_proc == 'average':
                flag = 1
            elif rpt_proc == 'difference':
                flag = 2
        self._lib.nisdoct_configure_processing(
            bool(enabled),
            bool(subtract_background),
            bool(interp),
            float(intpdk),
            np.array(apod_window).astype(np.float32),
            a_rpt_proc_flag,
            b_rpt_proc_flag,
            int(n_frame_avg)
        )

    def set_scan(self, x, y, lt, ft):
        """Sets the signals used to drive the galvos and trigger camera and frame grabber. Can be called during a scan.

        Args:
            x (np.ndarray): X galvo drive signal
            y (np.ndarray): Y galvo drive signal
            lt (np.ndarray): Camera A-line exposure trigger signal
            ft (np.ndarray): Frame grabber trigger signal
        """
        self._lib.nisdoct_set_pattern(
            np.array(x).astype(np.float64),
            np.array(y).astype(np.float64),
            np.array(lt).astype(np.float64),
            np.array(ft).astype(np.float64),
            len(x)
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
        self._lib.nisdoct_start_acquisition(
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

    # def grab_spectrum(self, output):
    #     return self._lib.NIOCT_grabSpectrum(self._handle, output)
