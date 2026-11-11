package fixture;

import java.util.Iterator;

public class Cursor<E> implements Iterator<E>, MutableMarker {
    public static int allocations;
    public E value;
    public boolean consumed;
    public int nextCalls;
    public int removeCalls;
    public RuntimeException failure;

    public Cursor(int unused) {
        allocations++;
        this.value = null;
    }

    public boolean hasNext() {
        return !consumed;
    }

    public void remove() {
        removeCalls++;
        if (failure != null) throw failure;
        consumed = false;
    }

    public E next() {
        nextCalls++;
        consumed = true;
        if (failure != null) throw failure;
        return value;
    }
}
