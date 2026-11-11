package probe;
import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.reflect.*;
public final class UnusedPrivateIntersectionCaller {
    private static Owner receiver() { Trace.hit(7); Trace.receiverAction.run(); return Trace.current; }
    private static Object argument() { Trace.hit(8); Trace.argumentAction.run(); return Trace.input; }
    public static void call(int route, Method entry, Method helper, MethodHandle handle) throws Throwable {
        Owner receiver = receiver(); Object input = argument();
        if (route == 0) Owner.Dispatch.call(receiver, input);
        else if (route == 2) handle.invokeExact(receiver, input);
        else {
            try { (route == 1 ? entry : helper).invoke(null, receiver, input); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
    }
}
