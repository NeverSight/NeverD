package fixture;
public class Gate<T> {
    public Payload payload;
    public int count;
    public RuntimeException failure;
    public int leadSteps;
    public int tailSteps;
    public int leadCount;
    public int tailCount;
    public void update(Object lock, Runnable callback) {
        int before = leadSteps;
        int after = tailSteps;
        while (before-- > 0) {
            if (callback != null) { callback.run(); }
            leadCount++;
        }
        synchronized (lock) {
            if (payload == null) payload = new Payload(lock);
            if (callback != null) callback.run();
            int previous = count;
            count = previous + 1;
            if (failure != null) throw failure;
        }
        while (after-- > 0) {
            if (callback != null) { callback.run(); }
            tailCount++;
        }
    }
}
