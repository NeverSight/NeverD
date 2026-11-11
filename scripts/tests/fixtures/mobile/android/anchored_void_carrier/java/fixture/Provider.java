package fixture;
public class Provider<C> {
    public static <T> void send(Cell<T> cell, Callback<? super T,Boolean> callback,
                               Callback<? super String,Boolean> fixed, int value) {
        Trace.hit(1);
        if (cell != Trace.expectedCell || callback != Trace.expectedCallback || fixed != Trace.expectedFixed)
            throw new AssertionError("owned carrier identity");
        T item = cell.value;
        if (item != Trace.expectedInput) throw new AssertionError("owned item identity");
        boolean first = callback.apply(item).booleanValue();
        Trace.hit(3);
        boolean second = fixed.apply("owned").booleanValue();
        Trace.hit(5);
        if (first != second || value != 37) throw new AssertionError("owned callback results");
    }
}
