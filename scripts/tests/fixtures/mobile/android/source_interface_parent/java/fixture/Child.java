package fixture;

import fixture.base.Base;

public class Child<I, J> extends Base<I, J> {
    public Child() {}

    public I value(I value) {
        calls += 2;
        if (failure != null) throw failure;
        last = value;
        return value;
    }

    protected Cell<I, J> lookup(I key) {
        calls += 3;
        if (failure != null) throw failure;
        last = key;
        return stored;
    }
}
