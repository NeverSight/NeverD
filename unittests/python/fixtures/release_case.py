base = 7


def combine(left, right):
    value = left + right
    return value * left


def choose(left, right):
    if left:
        value = right + left
    else:
        value = right - left
    return value
