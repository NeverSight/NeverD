package fixture;

public class Receiver<C> implements Wrapper<C>, FormalView {
    public int calls;
    public Object last;
    public RuntimeException failure;

    public Receiver() {}

    public <T> T get(Key<T> key) {
        calls++;
        last = key;
        if (failure != null) throw failure;
        return key.value;
    }

    public Object fixed(Key<?> key) {
        calls++;
        last = key;
        if (failure != null) throw failure;
        return key.value;
    }

    public Object plain(Object key) {
        return Dispatch.plain(this, key);
    }
}
