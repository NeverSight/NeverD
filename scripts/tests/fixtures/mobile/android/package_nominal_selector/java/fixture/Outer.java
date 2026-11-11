package fixture;
public final class Outer {
    private Outer() {}
    public static int events;
    public static class Item<E> {
        public Object value;
        Item(Object value) { this.value = value; }
        public E get() { events = events * 5 + 4; return (E) value; }
    }
    static class Provider extends Base {
        Probe probe;
        RuntimeException failure;
        Object observed;
        Item result;
        int selected;
        <T> Item<T> select(int index) {
            events = events * 5 + 1;
            selected = index;
            Object value = probe.lookup(index);
            observed = value;
            Item<T> actual = (Item<T>) value;
            if (failure != null) throw failure;
            result = actual;
            events = events * 5 + 2;
            return actual;
        }
    }
}
