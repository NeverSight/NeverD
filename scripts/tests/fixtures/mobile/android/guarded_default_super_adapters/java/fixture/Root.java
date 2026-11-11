package fixture;

public class Root<R> implements CountView<R> {
    public int calls;
    public Object last;
    public Object backing;
    public RuntimeException failure;
    public Root() {}

    @SuppressWarnings("unchecked")
    public R get(Object value) {
        calls++;
        last = value;
        if (failure != null) throw failure;
        return (R) backing;
    }
}
