"""Optional structured-action extensions over NeverD's process protocol v1.

No Agent or model objects cross this interface. A continuation is provided by
the host from an exact earlier operation, including its original input context.
"""
from __future__ import annotations

from typing import Any, Callable
import neverd_gui_plugin as protocol


class Task(protocol.Task):
    def __init__(self, request, input_stream=None, output_stream=None):
        super().__init__(request, input_stream, output_stream)
        self.continuation = request.get("continuation")
        self.extensions = request.get("extensions", [])

    def command(self, operation: str, **payload: Any) -> dict[str, Any]:
        """Submit a declared command; return its complete host receipt.

        Cancellation does not undo a command already accepted by the project.
        The host also retains this receipt independently of the provider.
        """
        if "commands" not in self.extensions:
            raise RuntimeError("This host does not offer command extensions")
        return self._request("command", "command_reply", operation, payload)


def run(handler: Callable[[Task], dict[str, Any]]) -> int:
    return protocol.run(handler, task_type=Task)
