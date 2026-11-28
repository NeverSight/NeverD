"""Owned fixture for CPython 3.11 disk caches, locations and unwind tables."""


def classify(items, score):
    result = []
    for item in items:
        try:
            match item:
                case {"value": value} if value is not None:
                    result.append(score(value))
                case _:
                    continue
        except (ValueError, TypeError) as error:
            result.append(str(error))
        finally:
            score(None)
    return result


def factory(base):
    def apply(value):
        return value + base

    return apply


async def consume(source):
    async for value in source:
        yield value
