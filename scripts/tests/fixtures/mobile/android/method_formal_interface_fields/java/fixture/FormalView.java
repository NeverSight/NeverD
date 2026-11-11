package fixture;

public interface FormalView {
    int TOKEN = 11;
    <T> T get(Key<T> key);
    Object fixed(Key<?> key);
    Object plain(Object key);
}
