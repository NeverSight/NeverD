package fixture.base;

import fixture.Cell;
import fixture.Contract;

public class Base<P, Q> implements Contract<P> {
    public int calls;
    public P last;
    public RuntimeException failure;
    public Cell<P, Q> stored;

    public Base() {}

    public P value(P value) {
        calls++;
        if (failure != null) throw failure;
        last = value;
        return value;
    }

    protected Cell<P, Q> lookup(P key) {
        calls++;
        if (failure != null) throw failure;
        last = key;
        return stored;
    }

    public Cell<P, Q> fetch(P key) {
        return lookup(key);
    }
}
