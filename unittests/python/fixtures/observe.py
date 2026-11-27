"""Behavioral oracle for the owned fixture sources only."""
import asyncio
import json
import sys

namespace = {}
with open(sys.argv[1], "rb") as source:
    exec(compile(source.read(), "owned.py", "exec"), namespace)
seen = []
if "maker" in namespace:
    counter = namespace["maker"](5)
    seen.extend([counter(2), counter(-1), namespace["arithmetic"](4, scale=3)])
    seen.extend(namespace["choose"](v) for v in (-3, 4))
    seen.extend([namespace["walk"]([2, 3, 7]), namespace["positives"]([-1, 0, 3]),
                 list(namespace["ticks"]([0, 2]))])
    async def value():
        return 71
    seen.append(asyncio.run(namespace["wait_for"](value())))
    def fail():
        raise ValueError("expected")
    def propagate():
        raise LookupError("owned")
    for f in (lambda: 6, fail, propagate):
        log = []
        try:
            seen.append(namespace["guarded"](f, log))
        except LookupError:
            seen.append("propagated")
        seen.append(log)
    class Context:
        value = 11
        def __enter__(self):
            seen.append("enter")
            return self
        def __exit__(self, *args):
            seen.append(args[0].__name__ if args[0] else "normal")
            return False
    seen.append(namespace["using"](Context()))
    class ErrorContext(Context):
        @property
        def value(self):
            raise LookupError
    try:
        namespace["using"](ErrorContext())
    except LookupError:
        seen.append("with propagates")
    seen.extend([namespace["tagged"](0), namespace["tagged"](5), namespace["assigned"](6)])
elif "setting" in namespace:
    seen.extend([namespace["setting"](9), namespace["setting"].__doc__])
else:
    for condition in (0, 5):
        def first():
            seen.append("first")
            return condition
        def second():
            seen.append("second")
            return 6
        seen.append(namespace["conditional"](first, second))
    seen.extend(map(repr, namespace["constants"]()))
    async def collect():
        return [v async for v in namespace["stream"](8)]
    seen.append(asyncio.run(collect()))
    for fail in (False, True):
        def action():
            if fail:
                raise LookupError
            return 9
        log = []
        try:
            seen.append(namespace["cleanup"](action, log))
        except LookupError:
            seen.append("raised")
        seen.append(log)
print(json.dumps(seen, ensure_ascii=True))
