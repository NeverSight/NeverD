package fixture;
public class Core<T extends Bound> implements Api<T> {
    public int calls;
    public Bound observed;
    public boolean nullValue;
    public RuntimeException failure;
    public boolean matches(Bound value) {
        calls++; observed = value;
        if (failure != null) throw failure;
        return value == null ? nullValue : value.read();
    }
    public Object copy() { return this; }
}
