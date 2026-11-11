package fixture;

import fixture.base.Base;

public class Child<S extends Tag, U extends Tag> extends Base<S, S> {
    public int calls;
    public S last;
    public RuntimeException failure;

    public Child() {}

    public S value(S... values) {
        calls++;
        if (failure != null) throw failure;
        last = values[0];
        return last;
    }
}
