def conditional(first, second):
    return first() and second()

def constants():
    return (33554439, -4.25, b"\x00\xff", "橙")

async def stream(value):
    yield value

def cleanup(action, record):
    try:
        result = action()
    finally:
        record.append("done")
    return result
