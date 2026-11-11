import fixture.*;
public final class MonitorPostReleaseCaller {
    public static int update(Gate<?> gate, Object lock, Runnable inside, Runnable after, boolean enabled) {
        return gate.update(lock, inside, after, enabled);
    }
}
