package fixture;

public class Root<R,S> implements CountView<R> {
    public int calls;
    public Object last;
    public Element<R> expected;
    public RuntimeException failure;

    public Root() {}

    public boolean contains(Object value) {
        calls++;
        last = value;
        if (failure != null) throw failure;
        return value == expected;
    }
}
