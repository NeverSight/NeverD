package fixture;

public final class Core extends Base<java.util.List<Payload>> {
    public final int code;
    public final Object root, context;
    Core(Object root, int left, int right, java.util.List<Payload> values,
         Runnable action, Object context, java.util.Collection<Payload> extras, Object token) {
        super(values, action, extras, token);
        this.code = left * 31 + right;
        this.root = root;
        this.context = context;
        Trace.children++;
        Trace.stage = 4;
    }
    public Core(java.util.List<Payload> values, Runnable action,
                java.util.Collection<Payload> extras, Object token, int code) {
        this(token, code, 3, values, action, token, extras, token);
    }
    public int state() { return code; }
    public static Core make(Object root, int left, int right, java.util.List<Payload> values,
                            Runnable action, Object context, java.util.Collection<Payload> extras, Object token) {
        return new Core(root, left, right, values, action, context, extras, token);
    }
}
