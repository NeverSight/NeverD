package fixture;

import java.util.Iterator;

public class Cursor<E> implements Iterator<E> {
    public static int allocations;
    public E value;
    public boolean consumed;
    public int nextCalls;
    public RuntimeException failure;

    public Cursor(E value) {
        allocations++;
        this.value = value;
    }

    public boolean hasNext() {
        return !consumed;
    }

    public E next() {
        nextCalls++;
        consumed = true;
        if (failure != null) throw failure;
        return value;
    }
}
