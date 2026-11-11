package fixture;
public class Core extends Middle {
    public Core output;
    public Object seen, tag;
    public int calls;
    public RuntimeException failure;
    public Core get(Object marker) {
        calls++; seen = marker;
        if (failure != null) throw failure;
        return output;
    }
    protected Core protectedGet() {
        calls++;
        if (failure != null) throw failure;
        return output;
    }
    public <U> U identity(U value) { calls++; return value; }
}
