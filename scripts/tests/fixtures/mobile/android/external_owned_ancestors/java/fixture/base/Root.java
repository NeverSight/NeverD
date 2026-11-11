package fixture.base;

import fixture.Cell;

public class Root<R, S> {
    public int calls;
    public R last;
    public RuntimeException failure;
    public Cell<R, S> stored;

    public Root() {}

    public R value(R value) {
        calls++;
        if (failure != null) throw failure;
        last = value;
        return value;
    }

    protected Cell<R, S> lookup(R key) {
        calls++;
        if (failure != null) throw failure;
        last = key;
        return stored;
    }

    public Cell<R, S> fetch(R key) {
        return lookup(key);
    }
}
