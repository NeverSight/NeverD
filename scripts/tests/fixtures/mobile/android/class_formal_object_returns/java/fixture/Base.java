package fixture;
public abstract class Base<T> implements SourceView {
    public T source;
    public int gets;
    public int echoes;
    public int events;
    public Callback<String, byte[]> seen;
    public RuntimeException failure;
    public Base() {}
    public T getSource() {
        gets++;
        events = events * 10 + 1;
        if (failure != null) throw failure;
        return source;
    }
    public T echo(Callback<String, byte[]> callback) {
        echoes++;
        events = events * 10 + 2;
        seen = callback;
        if (failure != null) throw failure;
        return source;
    }
}
