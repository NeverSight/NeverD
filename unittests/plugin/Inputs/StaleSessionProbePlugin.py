import builtins

from neverd_plugin import Plugin, Session


@Plugin(name="Python Stale Probe", version="1.0.0")
class StaleSessionProbePlugin:
    def on_run(self, session: Session, arg: int) -> int:
        stale = builtins._neverd_stale_session
        try:
            _ = stale.file_path
        except RuntimeError as error:
            if "no longer active" not in str(error):
                raise
        else:
            raise AssertionError("unloaded plugin retained an active Session")
        del builtins._neverd_stale_session
        return 0
