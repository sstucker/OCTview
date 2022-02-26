import ctypes as c
import numpy as np


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
        self._lib.nisdoct_configure_processing(
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
            np.float64(intpdk),
            np.array(apod_window).astype(np.float32),
            a_rpt_proc_flag,
            b_rpt_proc_flag,
            int(n_frame_avg)
        )

    #
    # def is_scanning(self):
    #     """Returns bool indicating whether controller is actively imaging."""
    #     return self._lib.NIOCT_is_scanning(self._handle)
    #
    # def is_saving(self):
    #     """Returns bool indicating whether controller is streaming data to the disk."""
    #     return self._lib.NIOCT_is_saving(self._handle)
    #
    # def is_ready_to_scan(self):
    #     """Returns bool indicating whether controller is fully configured and ready to scan."""
    #     return self._lib.NIOCT_is_ready_to_scan(self._handle)
    #
    #
    # def set_scan(self, x, y, lt, ft):
    #     """Sets the signals used to drive the galvos and trigger camera and frame grabber. Can be called during a scan.
    #
    #     Args:
    #         x (np.ndarray): X galvo drive signal
    #         y (np.ndarray): Y galvo drive signal
    #         lt (np.ndarray): Camera A-line exposure trigger signal
    #         ft (np.ndarray): Frame grabber trigger signal
    #     """
    #     self._lib.NIOCT_setScan(self._handle, x, y, lt, ft, len(x))
    #
    # def start_scan(self):
    #     """Start imaging without streaming any data to disk."""
    #     self._lib.NIOCT_startScan(self._handle)
    #
    # def stop_scan(self):
    #     """Halt any imaging or streaming taking place."""
    #     self._lib.NIOCT_stopScan(self._handle)
    #
    # def start_save(self, file_name, max_bytes):
    #     """Begins streaming a TIFF file to disk until scanning stops or stop_save is called.
    #
    #     Args:
    #         file_name (str):Desired name of output file
    #         max_bytes (int):Maximum size each file can be before a new one is created
    #     """
    #     self._lib.NIOCT_startSave(self._handle, file_name.split('.')[0].encode('utf-8'), int(max_bytes))
    #
    # def save_n(self, file_name, max_bytes, frames_to_save):
    #     """Streams frames_to_save frames to disk.
    #
    #     Arg:
    #         file_name (str): Desired name of output file.
    #         max_bytes (int): Maximum size each file can be before a new one is created.
    #         frames_to_save (int): Number of frames to save.
    #     """
    #     self._lib.NIOCT_saveN(self._handle, file_name.split('.')[0].encode('utf-8'), int(max_bytes), int(frames_to_save))
    #
    # def stop_save(self):
    #     """Stop streaming data to disk."""
    #     self._lib.NIOCT_stopSave(self._handle)
    #
    # def grab_frame(self, output):
    #     return self._lib.NIOCT_grabFrame(self._handle, output)
    #
    # def grab_spectrum(self, output):
    #     return self._lib.NIOCT_grabSpectrum(self._handle, output)
