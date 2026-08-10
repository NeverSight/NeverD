from neverd_plugin import Plugin, Session


@Plugin(name="Python Raising Fixture", version="1.0.0")
class RaisingPlugin:
    def on_run(self, session: Session, arg: int) -> int:
        raise ValueError(f"intentional failure {arg}")

    def on_term(self) -> None:
        raise RuntimeError("intentional termination failure")
