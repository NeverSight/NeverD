package fixture;

public class Provider implements java.io.Serializable {
    public Provider() {}

    @SuppressWarnings("unchecked")
    public static <T> T readStatic(T seed, Class<? extends Runnable> type) {
        Trace.calls++;
        Trace.stage = 1;
        Trace.seed = seed;
        Trace.type = type;
        Trace.name = type.getName();
        Trace.stage = 2;
        Trace.action.run();
        Trace.stage = 3;
        return (T) Trace.value;
    }

    public <R> R read(R seed, Class<? extends Runnable> type) {
        Trace.instances++;
        Trace.receiver = this;
        return readStatic(seed, type);
    }
}
