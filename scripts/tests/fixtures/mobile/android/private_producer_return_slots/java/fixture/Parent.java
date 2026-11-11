package fixture;
public class Parent<T> extends Ancestor<T> implements Api<T> {
    public Leaf<T> value;
    public int calls;
    public RuntimeException failure;
    public Parent() {}
    private final Leaf<T> produce() {
        calls++;
        if (failure != null) throw failure;
        return value;
    }
    @SuppressWarnings("unchecked")
    public final Base<T> items() { return (Base<T>) (Object) produce(); }
    public Leaf<T> narrowItems() { return produce(); }
}
