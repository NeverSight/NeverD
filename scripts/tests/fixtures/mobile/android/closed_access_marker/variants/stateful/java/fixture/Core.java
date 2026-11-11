package fixture;
import java.util.Set;
public final class Core {
    public static int choose(int index) {
        switch (Kind.values()[index]) {
            case FIRST: return 11;
            default: return 22;
        }
    }
    public static Core latest;
    public final Set<Integer> values;
    public final Context context;
    public final Probe probe;
    public Object marker;
    public int stage;
    private Core(Set<Integer> values, Context context, Probe probe) {
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
        public static Core create(Set<Integer> values, Context context, Probe probe) {
            return new Core(values, context, probe);
        }
    }
}
