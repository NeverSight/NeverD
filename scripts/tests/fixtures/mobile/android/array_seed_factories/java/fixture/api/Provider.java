package fixture.api;
import fixture.Trace;
import java.util.List;
class Provider {
    static { Trace.hit(1); }
    @SuppressWarnings("unchecked")
    public static <T> List<T> make(T[] values) {
        Trace.seen = values; Trace.hit(2);
        Trace.length = values.length;
        Trace.hit(3);
        values[0] = (T) Trace.replacement;
        Trace.hit(4);
        return (List<T>) Trace.result;
    }
}
