package fixture;

public abstract class Base<S extends Base<S>> {
    public Object value;
    public int reads;
    public RuntimeException failure;
    protected Base(Object value) { this.value = value; }
    public Object read() {
        ++reads;
        if (failure != null) throw failure;
        return value;
    }
}
