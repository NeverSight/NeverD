package fixture;
public abstract class Base<K, V> implements Contract<K, V> {
    public Callback<K, V> current;
    public int listeners;
    public int fires;
    public RuntimeException failure;
    public Base() {}
    public Base<K, V> listener(Callback<K, V> callback) {
        listeners++;
        Trace.events = Trace.events * 10 + 1;
        current = callback;
        if (failure != null) throw failure;
        return this;
    }
    public void fire(K key, V value) {
        fires++;
        Trace.events = Trace.events * 10 + 2;
        removed(key, value);
        if (current != null) current.removed(key, value);
    }
    protected abstract void removed(K key, V value);
}
