package fixture;
import java.util.Set;
public final class Core implements Probe {
    private interface Handler { Object apply(Object value); }
    public static Object observed, result;
    public static int calls;
    public static Runnable action;
    public static Object implementation = new Handler() {
        public Object apply(Object value) {
            observed = value;
            calls++;
            if (action != null) action.run();
            return result;
        }
    };
    private final Handler handler;
    public Object observe(Core self, Object values, Context context) { return values; }
    public static Core latest;
    public final Set<Integer> values;
    public final Context context;
    public final Probe probe;
    public Object marker;
    public int stage;
    private Core(Set<Integer> values, Handler handler, Context context, Probe probe) {
        latest = this;
        this.values = values;
        this.handler = handler;
        this.context = context;
        this.probe = probe;
        stage = 1;
        marker = probe.observe(this, values, context);
        stage = 2;
        if (context != null && context.failure != null) throw context.failure;
        stage = 3;
    }
    public static final class Builder {
        private Builder() { }
        public static Core create(Set<Integer> values, Context context, Probe probe) {
            return new Core(values, (Handler) implementation, context, probe);
        }
    }
}
