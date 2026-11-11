package fixture;

public final class Worker<T, U> extends PlainState implements java.util.function.Consumer<T> {
    public T value;
    public int calls;
    public RuntimeException failure;

    public Worker(boolean initial, java.util.List<String> entries,
                  java.util.function.Consumer<String> callback) {
        super(initial);
        this.entries = entries;
        this.callback = callback;
        callback.accept("owned construction");
    }

    private final java.util.List<String> entries;
    private final java.util.function.Consumer<String> callback;
    public java.util.List<String> metadata() { return entries; }
    public void notify(String item) { callback.accept(item); }

    public void accept(T item) {
        if (compareAndSet(false, true)) {
            value = item;
            calls++;
            if (failure != null) throw failure;
        }
    }

    public String toString() { return "owned state"; }
}
