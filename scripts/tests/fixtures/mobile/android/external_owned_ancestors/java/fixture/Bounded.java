package fixture;

import fixture.base.Base;

public class Bounded<S extends Tag, U> extends Base<S, U> {
    public Bounded() {}

    public S value(S value) {
        calls += 4;
        if (failure != null) throw failure;
        last = value;
        return value;
    }
}
