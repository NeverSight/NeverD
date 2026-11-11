package fixture.api;
import fixture.*;
class Provider {
    static { Trace.hit(1); }
    public static final <T> Carrier<T> relay(Carrier<? super T> input) {
        Trace.hit(2);
        Trace.seen = input.value;
        input.value = (T) Trace.seed;
        Trace.hit(3);
        return (Carrier<T>) Trace.result;
    }
}
