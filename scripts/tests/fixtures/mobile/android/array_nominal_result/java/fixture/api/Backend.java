package fixture.api;
import fixture.Trace;
import fixture.Receipt;
class Backend {
    static { Trace.hit(1, null); }
    public static <T> Receipt collect(T[] values) {
        Trace.seen = values;
        Trace.hit(2, values);
        Trace.length = values.length;
        if (Trace.write) values[0] = (T)Trace.replacement;
        Trace.hit(3, values);
        Receipt chosen = Trace.result;
        Trace.hit(4, chosen);
        return chosen;
    }
    public static Receipt collect(Object value) {
        Trace.hit(8, value);
        return Trace.alternate;
    }
}
