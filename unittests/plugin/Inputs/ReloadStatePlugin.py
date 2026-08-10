from neverd_plugin import Plugin, Session


@Plugin(name="Python Reload State", version="1.0.0")
class ReloadStatePlugin:
    def __init__(self) -> None:
        self.calls = 0

    def on_run(self, session: Session, arg: int) -> int:
        self.calls += 1
        return self.calls
