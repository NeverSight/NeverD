"""Ownership-safe access to the already-loaded NeverD C ABI."""

from __future__ import annotations

import ctypes
import sys
from typing import Any, Protocol, cast

from . import abi


class HostUnavailableError(RuntimeError):
    """Raised when a host-only operation is used outside NeverD."""


class _NativeBridge(Protocol):
    def library_path(self) -> object: ...


def _host_library_path() -> str:
    try:
        import _neverd_plugin as native_module  # type: ignore[import-not-found]
    except ModuleNotFoundError as error:
        raise HostUnavailableError(
            "NeverD's native Python plugin bridge is unavailable; "
            "use this operation from a Python-enabled NeverD plugin"
        ) from error
    native = cast(_NativeBridge, native_module)
    path = native.library_path()
    if not isinstance(path, str) or not path:
        raise HostUnavailableError(
            "NeverD's native Python plugin bridge returned no library path"
        )
    return path


class HostAPI:
    """Lazy, typed binding to one loaded ``libneverd`` instance."""

    __slots__ = ("_library", "_functions")

    def __init__(self, library: object | None = None) -> None:
        self._library = (
            library if library is not None else ctypes.CDLL(_host_library_path())
        )
        self._functions: dict[str, Any] = {}

    def function(self, name: str) -> Any:
        """Return a configured ctypes function from the declared public ABI."""

        try:
            return self._functions[name]
        except KeyError:
            pass
        try:
            spec = abi.FUNCTION_SPECS[name]
        except KeyError as error:
            raise AttributeError(
                f"NeverD has no declared C API function {name!r}"
            ) from error
        try:
            function = getattr(self._library, name)
        except AttributeError as error:
            raise RuntimeError(
                f"loaded NeverD library does not export declared function {name}"
            ) from error
        function.restype = spec.restype
        function.argtypes = list(spec.argtypes)
        self._functions[name] = function
        return function

    def call(self, name: str, *arguments: object) -> object:
        """Call one raw C ABI function with its configured ctypes signature."""

        return self.function(name)(*arguments)

    def owned_string(self, name: str, *arguments: object) -> str | None:
        """Copy a UTF-8 C result and release it exactly once."""

        try:
            spec = abi.FUNCTION_SPECS[name]
        except KeyError as error:
            raise AttributeError(
                f"NeverD has no declared C API function {name!r}"
            ) from error
        if spec.ownership is not abi.Ownership.OWNED_STRING:
            raise TypeError(f"{name} does not return an owned NeverD string")
        pointer = self.function(name)(*arguments)
        address = int(pointer or 0)
        if not address:
            return None
        try:
            return ctypes.string_at(address).decode("utf-8", errors="strict")
        finally:
            self.function("neverd_free_string")(
                ctypes.cast(ctypes.c_void_p(address), ctypes.c_char_p)
            )

    def borrowed_bytes(self, name: str, *arguments: object) -> bytes:
        """Copy a borrowed ``unsigned char`` buffer with a trailing length out-param."""

        try:
            spec = abi.FUNCTION_SPECS[name]
        except KeyError as error:
            raise AttributeError(
                f"NeverD has no declared C API function {name!r}"
            ) from error
        if spec.ownership is not abi.Ownership.BORROWED_BUFFER:
            raise TypeError(f"{name} does not return a borrowed NeverD buffer")
        length = ctypes.c_ulonglong()
        pointer = self.function(name)(*arguments, ctypes.byref(length))
        if length.value > sys.maxsize:
            raise OverflowError(f"{name} returned an impossibly large buffer")
        if length.value == 0:
            return b""
        if not pointer:
            raise RuntimeError(f"{name} returned a null buffer with non-zero length")
        return bytes(ctypes.string_at(pointer, int(length.value)))


__all__ = ["HostAPI", "HostUnavailableError"]
