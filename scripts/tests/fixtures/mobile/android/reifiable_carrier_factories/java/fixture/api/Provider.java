package fixture.api;
import fixture.Trace;
import java.util.List;
class Provider {
    static { Trace.hit(1); }
    @SuppressWarnings("unchecked")
    public static <T> List<T> make(java.util.Collection<?> collection) {
        Trace.hit(2);
        Trace.seen = collection;
        ((int[]) Trace.input)[0]++;
        Trace.hit(3);
        return (List<T>) Trace.result;
    }
}
