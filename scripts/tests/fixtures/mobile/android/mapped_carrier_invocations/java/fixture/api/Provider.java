package fixture.api;
import fixture.*;
class Provider {
    static { Trace.hit(1); }
    public static <I,O> Box<O> make(Box<? extends I> seed, Fn<? super I, ? extends O> callback, long flags) {
        Trace.flags = flags; Trace.hit(2);
        O result = callback.apply(seed.value);
        Trace.callbackResult = result; Trace.hit(4);
        return (Box<O>)Trace.box;
    }
}
