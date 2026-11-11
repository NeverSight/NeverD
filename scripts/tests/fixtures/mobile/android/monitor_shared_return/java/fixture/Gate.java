package fixture;
public class Gate<T> {
    public Payload payload;
    public int count;
    public boolean bypass;
    public RuntimeException failure;
    public void update(Object lock, Runnable callback) {
        if (bypass) {
            if (callback != null) callback.run();
        } else if (callback != null) synchronized (lock) {
            if (payload == null) payload = new Payload(lock);
            if (callback != null) callback.run();
            int previous = count;
            count = previous + 1;
            if (failure != null) throw failure;
        }
    }
}
