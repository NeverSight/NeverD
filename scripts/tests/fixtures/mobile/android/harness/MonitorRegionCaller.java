import fixture.*;
public final class MonitorRegionCaller {
    public static void update(Gate<?> gate, Object lock, Runnable callback) { gate.update(lock, callback); }
}
