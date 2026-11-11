package fixture;
import java.util.Set;
final class Core {
    static final class Value {
        final int number;
        Value(int number) { this.number = number; }
        int intValue() { return number; }
    }
    public static Core latest;
    public final Set<Value> values;
    public final Context context;
    public final Probe probe;
    public Object marker;
    public int stage;
    private Core(Set<Value> values, Context context, Probe probe) {
        latest = this;
        this.values = values;
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
        public static Core create(Set<Value> values, Context context, Probe probe) {
            return new Core(values, context, probe);
        }
    }
}
