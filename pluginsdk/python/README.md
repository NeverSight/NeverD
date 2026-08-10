# NeverD Python Plugin SDK

`neverd-plugin` is the official, typed authoring package for Python plugins
loaded by NeverD. The package is pure Python and imports outside the host, so
editors, type checkers, and unit tests do not need a local NeverD library.

```python
from neverd_plugin import Event, Plugin, Session


@Plugin(name="My Plugin", version="1.0.0", author="Your team")
class MyPlugin:
    def on_init(self, session: Session) -> None:
        print(session.architecture)

    def on_run(self, session: Session, arg: int) -> int:
        return arg

    def on_event(self, event: Event) -> None:
        print(event.type.name)

    def on_term(self) -> None:
        pass
```

Install for authoring with `python -m pip install neverd-plugin`. At runtime,
NeverD stages a matching copy beside `libneverd` and injects a private native
bridge. Host-backed `Session` calls outside that context fail with an actionable
exception instead of loading an arbitrary library.

Python 3.10 or newer is supported. This package and NeverD are licensed under
the GNU Affero General Public License, version 3 only.
