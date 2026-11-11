package fixture;
public final class Gate<T> {
    public Payload payload;
    public int count;
    public RuntimeException failure;
    public int update(Object lock, Runnable callback, Runnable after, boolean enabled) {
        synchronized (lock) {
            if (payload == null) payload = new Payload(lock);
            if (callback != null) callback.run();
            count = count + 1;
            if (failure != null) throw failure;
            if (enabled && after != null) after.run();
        }
        return count;
    }
}
