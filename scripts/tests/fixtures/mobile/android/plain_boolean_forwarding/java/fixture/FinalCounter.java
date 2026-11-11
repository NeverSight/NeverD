package fixture;
public final class FinalCounter implements Api<Object> {
    public Object seen;
    public boolean selected;
    public int calls;
    public RuntimeException failure;
    public boolean add(Object value) { return addImpl(value); }
    public boolean addImpl(Object value) {
        calls++; seen = value;
        if (failure != null) throw failure;
        return selected;
    }
}
