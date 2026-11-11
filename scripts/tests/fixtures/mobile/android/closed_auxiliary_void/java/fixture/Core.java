package fixture;
public final class Core {
    private Core() {}
    public static void run(Object owner, Object type, Object action) {
        Registry registry = (Registry)owner; Token token = (Token)type; Callback callback = (Callback)action;
        Trace.hit(0, registry, token, callback, null);
        registry.consume(token, callback);
        Trace.hit(4, registry, token, callback, null);
    }
    public static void typed(Object owner, Object type, Object action) {
        Contract registry = (Contract)owner; Token token = (Token)type; Callback callback = (Callback)action;
        Trace.hit(0, registry, token, callback, null);
        registry.consume(token, callback);
        Trace.hit(4, registry, token, callback, null);
    }
}
