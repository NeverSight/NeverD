package fixture;

public abstract class Parent<T extends Bound> implements Source {
    public T source;
    public int gets;
    public int echoes;
    public int events;
    public int tag;
    public Callback<String, byte[]> seen;
    public RuntimeException failure;

    public Parent() {}

    @Override
    public Class<? extends Bound> type() {
        return Token.class;
    }

    @Override
    public T get() {
        gets++;
        events = events * 10 + 1;
        Trace.events = Trace.events * 10 + tag;
        if (failure != null) throw failure;
        return source;
    }

    @Override
    public T echo(Callback<String, byte[]> callback) {
        echoes++;
        events = events * 10 + 2;
        seen = callback;
        if (failure != null) throw failure;
        return source;
    }
}
