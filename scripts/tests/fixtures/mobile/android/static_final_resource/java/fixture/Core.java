package fixture;
public final class Core {
    private Core() {}
    public static final Object VALUE;
    static {
        try (Provider resource = Trace.create()) {
            VALUE = resource.get();
        }
    }
    public static Object value() { return VALUE; }
}
