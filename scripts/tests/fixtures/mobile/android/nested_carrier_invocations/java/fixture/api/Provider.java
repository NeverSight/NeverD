package fixture.api;
import fixture.*;
class Provider {
    static { Trace.hit(1); }
    public static <T> Box<T> make(Fn<? super Port<? super T>, ? super Sink<? super Token>, ? extends Object> callback, long flags) {
        Trace.flags = flags; Trace.hit(2);
        Object result = callback.apply((Port<? super T>)(Port)Trace.port, (Sink<? super Token>)(Sink)Trace.sink);
        Trace.callbackResult = result;
        return (Box<T>)Trace.box;
    }
}
