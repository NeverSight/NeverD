package fixture;
public class Context {
    public Object marker;
    public RuntimeException failure;
    public int calls;
    public Object touch() { calls++; if (failure != null) throw failure; return marker; }
}
