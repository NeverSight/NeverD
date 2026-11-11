package fixture;

import fixture.base.Base;

public class Closed extends Base<Object, Tag> {
    public int calls;
    public Object first;
    public Tag result;
    public RuntimeException failure;

    public Closed() {}

    public Tag value(Object... values) {
        calls++;
        if (failure != null) throw failure;
        first = values[0];
        return result;
    }
}
