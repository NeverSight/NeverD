package fixture;
public interface Callback {
    void locked(Object lock, Object seed) throws Exception;
    Object unlocked(Object lock, Object seed) throws Exception;
    Object recover(Object lock, Object seed, Recoverable failure) throws Exception;
    void observe(Object lock, Object seed, Exception failure) throws Exception;
}
