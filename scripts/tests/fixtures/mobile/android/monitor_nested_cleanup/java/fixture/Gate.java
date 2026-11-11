package fixture;
public class Gate<T> {
    public Payload payload;
    public int count;
    public RuntimeException failure;
    public void update(Object lock, Runnable callback) {
        synchronized (lock) {
            try {
            if (payload == null) payload = new Payload(lock);
            if (callback != null) callback.run();
            int previous = count;
            count = previous + 1;
            if (failure != null) throw failure;
            } catch (Throwable original) {
                try {
                    Cleanup.hit(1);
                    try { Cleanup.hit(2); }
                    finally { Cleanup.hit(3); }
                } finally { Cleanup.hit(4); }
                throw original;
            }
        }
    }
}
