#!/usr/bin/env python3
"""Minimal headless libretro frontend used by the 32X end-to-end tests."""
from __future__ import annotations

import binascii
import ctypes as C
import os
from pathlib import Path
import struct
from typing import Iterable
import zlib

RETRO_DEVICE_JOYPAD = 1
RETRO_PIXEL_FORMAT_0RGB1555 = 0
RETRO_PIXEL_FORMAT_XRGB8888 = 1
RETRO_PIXEL_FORMAT_RGB565 = 2

# Environment commands used by PicoDrive and most software libretro cores.
ENV_GET_CAN_DUPE = 3
ENV_SET_MESSAGE = 6
ENV_SHUTDOWN = 7
ENV_SET_PERFORMANCE_LEVEL = 8
ENV_GET_SYSTEM_DIRECTORY = 9
ENV_SET_PIXEL_FORMAT = 10
ENV_SET_INPUT_DESCRIPTORS = 11
ENV_GET_VARIABLE = 15
ENV_SET_VARIABLES = 16
ENV_GET_VARIABLE_UPDATE = 17
ENV_SET_SUPPORT_NO_GAME = 18
ENV_GET_LOG_INTERFACE = 27
ENV_GET_SAVE_DIRECTORY = 31
ENV_SET_CONTROLLER_INFO = 35
ENV_SET_MEMORY_MAPS = 36
ENV_GET_INPUT_BITMASKS = 51
ENV_GET_CORE_OPTIONS_VERSION = 52
ENV_SET_CORE_OPTIONS = 53
ENV_SET_CORE_OPTIONS_INTL = 54
ENV_SET_CORE_OPTIONS_DISPLAY = 55
ENV_GET_DISK_CONTROL_INTERFACE_VERSION = 57
ENV_SET_DISK_CONTROL_EXT_INTERFACE = 58
ENV_GET_MESSAGE_INTERFACE_VERSION = 59
ENV_SET_MESSAGE_EXT = 60
ENV_GET_THROTTLE_STATE = 71


class RetroGameInfo(C.Structure):
    _fields_ = [
        ("path", C.c_char_p),
        ("data", C.c_void_p),
        ("size", C.c_size_t),
        ("meta", C.c_char_p),
    ]


EnvironmentCB = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)
VideoCB = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
AudioCB = C.CFUNCTYPE(None, C.c_int16, C.c_int16)
AudioBatchCB = C.CFUNCTYPE(C.c_size_t, C.POINTER(C.c_int16), C.c_size_t)
InputPollCB = C.CFUNCTYPE(None)
InputStateCB = C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)


class LibretroHarness:
    def __init__(self, core: os.PathLike[str] | str, rom: os.PathLike[str] | str):
        self.core_path = Path(core).resolve()
        self.rom_path = Path(rom).resolve()
        self.core = C.CDLL(str(self.core_path))
        self.pixel_format = RETRO_PIXEL_FORMAT_0RGB1555
        self.buttons: set[int] = set()
        self.buttons2: set[int] = set()   # libretro port 1 = 32X pad 2
        self.frame_count = 0
        self.width = 0
        self.height = 0
        self.pitch = 0
        self.frame_raw = b""
        self.shutdown_requested = False
        self._dir_bytes = str(self.rom_path.parent).encode()
        self._wire_api()

    def _wire_api(self) -> None:
        self._environment_cb = EnvironmentCB(self._environment)
        self._video_cb = VideoCB(self._video)
        self._audio_cb = AudioCB(lambda _left, _right: None)
        self._audio_batch_cb = AudioBatchCB(lambda _data, frames: frames)
        self._input_poll_cb = InputPollCB(lambda: None)
        self._input_state_cb = InputStateCB(self._input_state)

        self.core.retro_set_environment.argtypes = [EnvironmentCB]
        self.core.retro_set_video_refresh.argtypes = [VideoCB]
        self.core.retro_set_audio_sample.argtypes = [AudioCB]
        self.core.retro_set_audio_sample_batch.argtypes = [AudioBatchCB]
        self.core.retro_set_input_poll.argtypes = [InputPollCB]
        self.core.retro_set_input_state.argtypes = [InputStateCB]
        self.core.retro_set_controller_port_device.argtypes = [C.c_uint, C.c_uint]
        self.core.retro_load_game.argtypes = [C.POINTER(RetroGameInfo)]
        self.core.retro_load_game.restype = C.c_bool
        self.core.retro_api_version.restype = C.c_uint

        self.core.retro_set_environment(self._environment_cb)
        self.core.retro_set_video_refresh(self._video_cb)
        self.core.retro_set_audio_sample(self._audio_cb)
        self.core.retro_set_audio_sample_batch(self._audio_batch_cb)
        self.core.retro_set_input_poll(self._input_poll_cb)
        self.core.retro_set_input_state(self._input_state_cb)
        assert self.core.retro_api_version() == 1
        self.core.retro_init()
        self.core.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD)
        self.core.retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD)

        info = RetroGameInfo(str(self.rom_path).encode(), None, 0, None)
        if not self.core.retro_load_game(C.byref(info)):
            self.core.retro_deinit()
            raise RuntimeError(f"core rejected ROM: {self.rom_path}")

    def _environment(self, command: int, data: int) -> bool:
        if command in (ENV_GET_SYSTEM_DIRECTORY, ENV_GET_SAVE_DIRECTORY):
            C.cast(data, C.POINTER(C.c_char_p))[0] = self._dir_bytes
            return True
        if command == ENV_SET_PIXEL_FORMAT:
            self.pixel_format = C.cast(data, C.POINTER(C.c_int))[0]
            return self.pixel_format in (0, 1, 2)
        if command in (ENV_GET_CAN_DUPE, ENV_GET_INPUT_BITMASKS):
            C.cast(data, C.POINTER(C.c_bool))[0] = True
            return True
        if command == ENV_GET_CORE_OPTIONS_VERSION:
            C.cast(data, C.POINTER(C.c_uint))[0] = 2
            return True
        if command == ENV_GET_MESSAGE_INTERFACE_VERSION:
            C.cast(data, C.POINTER(C.c_uint))[0] = 1
            return True
        if command == ENV_GET_DISK_CONTROL_INTERFACE_VERSION:
            C.cast(data, C.POINTER(C.c_uint))[0] = 1
            return True
        if command == ENV_GET_VARIABLE_UPDATE:
            C.cast(data, C.POINTER(C.c_bool))[0] = False
            return True
        if command == ENV_SHUTDOWN:
            self.shutdown_requested = True
            return True
        if command in (
            ENV_SET_MESSAGE,
            ENV_SET_MESSAGE_EXT,
            ENV_SET_PERFORMANCE_LEVEL,
            ENV_SET_INPUT_DESCRIPTORS,
            ENV_SET_VARIABLES,
            ENV_SET_SUPPORT_NO_GAME,
            ENV_SET_CONTROLLER_INFO,
            ENV_SET_MEMORY_MAPS,
            ENV_SET_CORE_OPTIONS,
            ENV_SET_CORE_OPTIONS_INTL,
            ENV_SET_CORE_OPTIONS_DISPLAY,
            ENV_SET_DISK_CONTROL_EXT_INTERFACE,
        ):
            return True
        if command in (ENV_GET_VARIABLE, ENV_GET_LOG_INTERFACE, ENV_GET_THROTTLE_STATE):
            return False
        return False

    def _video(self, data: int, width: int, height: int, pitch: int) -> None:
        # NULL means a duplicated frame. -1 is the HW framebuffer sentinel.
        if data and data != C.c_void_p(-1).value:
            self.width = width
            self.height = height
            self.pitch = pitch
            self.frame_raw = C.string_at(data, pitch * height)
        self.frame_count += 1

    def _input_state(self, port: int, device: int, index: int, button_id: int) -> int:
        del index
        if device != RETRO_DEVICE_JOYPAD:
            return 0
        if port == 0:
            return int(button_id in self.buttons)
        if port == 1:
            return int(button_id in self.buttons2)
        return 0

    def run(self, frames: int, buttons: Iterable[int] = (),
            buttons2: Iterable[int] = ()) -> None:
        self.buttons = set(buttons)
        self.buttons2 = set(buttons2)
        for _ in range(frames):
            self.core.retro_run()
            if self.shutdown_requested:
                raise RuntimeError("core requested shutdown")

    def rgb_frame(self) -> bytes:
        if not self.frame_raw:
            raise RuntimeError("no video frame received")
        rgb = bytearray(self.width * self.height * 3)
        src = self.frame_raw
        out = 0
        for y in range(self.height):
            row = y * self.pitch
            for x in range(self.width):
                if self.pixel_format == RETRO_PIXEL_FORMAT_XRGB8888:
                    value = int.from_bytes(src[row + x * 4:row + x * 4 + 4], "little")
                    r, g, b = (value >> 16) & 255, (value >> 8) & 255, value & 255
                else:
                    value = src[row + x * 2] | (src[row + x * 2 + 1] << 8)
                    if self.pixel_format == RETRO_PIXEL_FORMAT_RGB565:
                        r = ((value >> 11) & 31) * 255 // 31
                        g = ((value >> 5) & 63) * 255 // 63
                        b = (value & 31) * 255 // 31
                    else:
                        r = ((value >> 10) & 31) * 255 // 31
                        g = ((value >> 5) & 31) * 255 // 31
                        b = (value & 31) * 255 // 31
                rgb[out:out + 3] = bytes((r, g, b))
                out += 3
        return bytes(rgb)

    def pixel(self, x: int, y: int) -> tuple[int, int, int]:
        """Read one output pixel without converting the complete frame."""
        if not self.frame_raw:
            raise RuntimeError("no video frame received")
        row = y * self.pitch
        if self.pixel_format == RETRO_PIXEL_FORMAT_XRGB8888:
            value = int.from_bytes(self.frame_raw[row + x * 4:row + x * 4 + 4], "little")
            return (value >> 16) & 255, (value >> 8) & 255, value & 255
        value = self.frame_raw[row + x * 2] | (self.frame_raw[row + x * 2 + 1] << 8)
        if self.pixel_format == RETRO_PIXEL_FORMAT_RGB565:
            return (
                ((value >> 11) & 31) * 255 // 31,
                ((value >> 5) & 63) * 255 // 63,
                (value & 31) * 255 // 31,
            )
        return (
            ((value >> 10) & 31) * 255 // 31,
            ((value >> 5) & 31) * 255 // 31,
            (value & 31) * 255 // 31,
        )

    def save_ppm(self, path: os.PathLike[str] | str) -> None:
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(f"P6\n{self.width} {self.height}\n255\n".encode() + self.rgb_frame())

    def save_png(self, path: os.PathLike[str] | str) -> None:
        """Write an RGB PNG using only the Python standard library."""
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        rgb = self.rgb_frame()
        rows = b"".join(
            b"\0" + rgb[y * self.width * 3:(y + 1) * self.width * 3]
            for y in range(self.height)
        )

        def chunk(kind: bytes, payload: bytes) -> bytes:
            body = kind + payload
            return struct.pack(">I", len(payload)) + body + struct.pack(">I", binascii.crc32(body) & 0xFFFFFFFF)

        png = b"\x89PNG\r\n\x1a\n"
        png += chunk(b"IHDR", struct.pack(">IIBBBBB", self.width, self.height, 8, 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(rows, 9))
        png += chunk(b"IEND", b"")
        path.write_bytes(png)

    def close(self) -> None:
        if self.core is not None:
            self.core.retro_unload_game()
            self.core.retro_deinit()
            self.core = None

    def __enter__(self) -> "LibretroHarness":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()
