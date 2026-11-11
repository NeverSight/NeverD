package fixture;
public final class Core {
    private Core() {}
    public static Object apply(Object lock, Callback callback, Object seed) throws Exception {
        synchronized (lock) {
            callback.locked(lock, seed);
        }
        try {
            return callback.unlocked(lock, seed);
        } catch (Recoverable failure) {
            return callback.recover(lock, seed, failure);
        } catch (Exception failure) {
            callback.observe(lock, seed, failure);
            throw failure;
        }
    }
}
