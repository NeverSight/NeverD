package fixture;
public final class Box {
    public Object value;
    public RuntimeException failure;
    public int calls;
    public Object read() { calls++; if (failure != null) throw failure; return value; }
}
