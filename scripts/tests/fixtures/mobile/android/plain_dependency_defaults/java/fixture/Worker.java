package fixture;

public final class Worker<T, U> extends PlainState implements java.util.function.Consumer<T> {
    public T value;
    public int calls;
    public RuntimeException failure;

    public Worker(boolean initial) { super(initial); }

    public void accept(T item) {
        if (compareAndSet(false, true)) {
            value = item;
            calls++;
            if (failure != null) throw failure;
        }
    }

    public String toString() { return "owned state"; }
}
