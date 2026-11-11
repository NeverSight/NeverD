package fixture.api;
import fixture.*;
class Base {
    public static final <A, B, R> Object invoke(
            Fn<? super A, ? super B, ? super Completion<? super R>, ? extends Object> callback,
            A left, B right, Completion<? super R> completion) {
        Trace.enter(left, right, completion);
        Object result = callback.apply(left, right, completion);
        Trace.tail(result);
        return result;
    }
}
