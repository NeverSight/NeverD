package fixture.api;
import fixture.*;
class Provider {
    static { Trace.hit(1); }
    public static <T> Box<T> make(T seed, Fn<? super T, ? extends T> callback, long flags) {
        Trace.flags = flags; Trace.hit(2);
        T result = callback.apply(seed);
        Trace.callbackResult = result; Trace.hit(4);
        return (Box<T>)Trace.box;
    }
}
