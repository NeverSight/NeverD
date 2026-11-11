package fixture.api;
import fixture.Trace;
import fixture.Box;
class Provider {
    static { Trace.hit(1); }
    @SuppressWarnings("unchecked")
    public static <E extends Enum<E>> Box<E> make(E[] values) {
        Trace.seen = values; Trace.hit(2);
        Trace.length = values.length;
        Trace.hit(3);
        values[0] = (E) Trace.replacement;
        Trace.hit(4);
        return (Box<E>) Trace.result;
    }
}
