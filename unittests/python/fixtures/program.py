import math
from operator import index as ordinal

def arithmetic(a, /, b=3, *, scale=2):
    return (a + b) * scale

def choose(value):
    if value < 0:
        return -value
    return value + 1

def walk(values):
    total = 0
    for value in values:
        total += value
    return total

def maker(start):
    count = start
    def advance(step):
        nonlocal count
        count += step
        return count
    return advance

def positives(values):
    return [value * 2 for value in values if value > 0]

def ticks(values):
    for value in values:
        yield value + 1

async def wait_for(task):
    return await task

def guarded(fn, log):
    try:
        result = fn()
    except ValueError:
        result = 7
    finally:
        log.append("closed")
    return result

def using(resource):
    with resource as item:
        result = item.value
    return result

def tagged(value):
    match value:
        case 0:
            return "zero"
        case _:
            return "other"

def assigned(value):
    return (n := value + 1) * n
