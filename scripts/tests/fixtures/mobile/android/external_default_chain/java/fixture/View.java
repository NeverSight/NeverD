package fixture;

public class View<T> implements Leaf<T>, Root<T> {
    public int calls;
    public int overloads;
    public RuntimeException failure;

    public View() {}

    public T value(T next) {
        calls++;
        if (failure != null) throw failure;
        return next;
    }

    public int fallback(int input, int other) {
        overloads++;
        if (failure != null) throw failure;
        return input + other;
    }
}
