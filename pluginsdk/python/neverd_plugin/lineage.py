"""Offline, evidence-preserving version comparison graphs through the C ABI."""

from __future__ import annotations

import json
import threading
from collections.abc import Callable, Iterator, Mapping, Sequence
from typing import Any, Literal, cast, overload

from . import abi
from .api import NeverDError
from .ffi import HostAPI

Progress = Callable[[str, int, int], None]


def _text(value: str) -> bytes:
    if not isinstance(value, str):
        raise TypeError("lineage text must be a string")
    if "\0" in value:
        raise ValueError("lineage text cannot contain a raw NUL")
    return value.encode("utf-8")


def _json(value: Mapping[str, Any]) -> bytes:
    return _text(json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False))


class Lineage:
    """One loaded collection with transactional replacement and bounded pages.

    Use as a context manager, or call ``close`` after operations have returned.
    Only ``cancel`` and ``status`` may overlap an active operation. Reports are
    retained evidence: connectivity establishes neither chronology nor patch
    presence. Engine settings and errors preserve the native API's meanings.
    """

    def __init__(self, *, host: HostAPI | None = None) -> None:
        self._host = host if host is not None else HostAPI()
        self._lock = threading.RLock()
        self._active = False
        self._handle = abi.LineageHandle()
        for name in ("create", "destroy", "cancel", "status_json", "last_error",
                     "load_json", "summary_json", "query_json", "export_json", "report_json"):
            self._host.function("neverd_lineage_" + name)
        self._handle = abi.LineageHandle(
            cast(int | None, self._host.call("neverd_lineage_create"))
        )
        if not self._handle.value:
            raise NeverDError("cannot allocate lineage handle")

    def _require(self) -> abi.LineageHandle:
        if not self._handle.value:
            raise NeverDError("lineage is closed")
        return self._handle

    def __enter__(self) -> Lineage:
        self._require()
        return self

    def __exit__(self, *args: object) -> None:
        self.close()

    def close(self) -> None:
        with self._lock:
            if self._active:
                raise NeverDError("cannot close lineage during an operation")
            if self._handle.value:
                self._host.call("neverd_lineage_destroy", self._handle)
                self._handle = abi.LineageHandle()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def cancel(self) -> None:
        with self._lock:
            self._host.call("neverd_lineage_cancel", self._require())

    def status(self) -> dict[str, Any]:
        with self._lock:
            text = self._host.owned_string("neverd_lineage_status_json", self._require())
        if text is None:
            raise NeverDError("cannot read lineage status")
        return cast(dict[str, Any], json.loads(text))

    @overload
    def _call(self, name: str, *args: object, progress: Progress | None = None,
              callback: bool = False, integer: Literal[False] = False) -> str: ...

    @overload
    def _call(self, name: str, *args: object, progress: Progress | None = None,
              callback: bool = False, integer: Literal[True]) -> int: ...

    def _call(self, name: str, *args: object, progress: Progress | None = None,
              callback: bool = False, integer: bool = False) -> str | int:
        with self._lock:
            handle = self._require()
            if self._active:
                raise NeverDError("lineage operation is already running")
            self._active = True
        errors: list[BaseException] = []

        def notify(_context: object, phase: bytes, done: int, total: int) -> None:
            if progress is None or errors:
                return
            try:
                progress(phase.decode("utf-8"), done, total)
            except BaseException as error:
                errors.append(error)
                self.cancel()

        try:
            cb = abi.LineageProgressCallback(notify) if progress else abi.LineageProgressCallback()
            tail = (cb, None) if callback else ()
            result: str | int | None
            if integer:
                result = cast(int, self._host.call(
                    "neverd_lineage_" + name, handle, *args, *tail,
                ))
            else:
                result = self._host.owned_string(
                    "neverd_lineage_" + name, handle, *args, *tail,
                )
            if errors:
                raise errors[0]
            if result is None or (integer and not result):
                message = self._host.owned_string("neverd_lineage_last_error", handle)
                raise NeverDError(message or "lineage operation failed")
            return result
        finally:
            with self._lock:
                self._active = False

    def load(self, document: str | Mapping[str, Any], *,
             options: Mapping[str, Any] | None = None,
             progress: Progress | None = None) -> Lineage:
        """Replace atomically; malformed/stopped admission retains the old data."""
        text = _text(document) if isinstance(document, str) else _json(document)
        self._call("load_json", text, _json(options or {}), progress=progress,
                   callback=True, integer=True)
        return self

    def load_reports(self, reports: Sequence[str], *,
                     options: Mapping[str, Any] | None = None,
                     progress: Progress | None = None) -> Lineage:
        """Combine exact saved JSON strings, keeping their original bytes."""
        if isinstance(reports, (str, bytes)) or any(not isinstance(r, str) for r in reports):
            raise TypeError("reports must be a sequence of saved report strings")
        return self.load({"schema": "neverd.bindiff.lineage", "schema_version": 1,
                          "reports": list(reports)}, options=options, progress=progress)

    def summary(self) -> dict[str, Any]:
        return cast(dict[str, Any], json.loads(self._call("summary_json")))

    def edit_releases(self, edit: Mapping[str, Any], *,
                      options: Mapping[str, Any] | None = None,
                      progress: Progress | None = None) -> Lineage:
        """Apply release metadata atomically; omitted revision uses the current one.

        Existing upserts may omit parents to retain them. Parent additions and
        removals use parent_updates without fetching the whole parent inventory.
        Requires the optional native release-edit export.
        """
        self._host.function("neverd_lineage_edit_releases_json")
        request = dict(edit)
        if "revision" not in request:
            request["revision"] = self.summary()["revision"]
        self._call("edit_releases_json", _json(request), _json(options or {}),
                   progress=progress, callback=True, integer=True)
        return self

    def query(self, kind: str, *, revision: str | None = None,
              progress: Progress | None = None, **options: Any) -> dict[str, Any]:
        if revision is None:
            revision = self.summary()["revision"]
        return cast(dict[str, Any], json.loads(self._call(
            "query_json", _json({**options, "kind": kind, "revision": revision}),
            progress=progress, callback=True,
        )))

    def pages(self, kind: str, **options: Any) -> Iterator[dict[str, Any]]:
        """Walk pages with one frozen revision; concurrent replacement refuses."""
        if options.get("revision") is None:
            options["revision"] = self.summary()["revision"]
        while True:
            page = self.query(kind, **options)
            yield page
            if page["next_offset"] is None:
                return
            options.pop("focus", None)
            options["offset"] = page["next_offset"]

    def export(self, *, progress: Progress | None = None) -> str:
        """Return the source-only persistence document, without truncation."""
        return self._call("export_json", progress=progress, callback=True)

    def report(self, report_id: str, *, progress: Progress | None = None) -> str:
        """Return the exact retained report, including its original formatting."""
        return self._call("report_json", _text(report_id), progress=progress, callback=True)

