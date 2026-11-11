package fixture;
public final class OwnedLocal extends ThreadLocal<String> {
    public String value;
    public int calls;
    public RuntimeException failure;
    protected String initialValue() {
        calls++;
        if (failure != null) throw failure;
        return value;
    }
    public String peek() { return initialValue(); }
}
