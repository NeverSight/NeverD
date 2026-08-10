from neverd_plugin import Event, Plugin, Session


@Plugin(
    name="Minimal Python Plugin",
    version="1.0.0",
    author="NeverD contributors",
    description="Smallest complete Python plugin example",
)
class MinimalPlugin:
    def on_init(self, session: Session) -> None:
        print(f"NeverD {session.version} initialized the plugin")

    def on_run(self, session: Session, arg: int) -> int:
        print(f"input={session.file_path!r}, argument={arg}")
        return 0

    def on_event(self, event: Event) -> None:
        print(f"event={event.type.name}")

    def on_term(self) -> None:
        print("plugin terminated")
