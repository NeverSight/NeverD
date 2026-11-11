package probe;
import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.reflect.*;
public final class FinalNestedVisibilityBridgeCaller {
    private static Owner.Child receiver() {
        Trace.hit(1); Trace.receiverAction.run(); return Trace.current;
    }
    private static Payload argument() {
        Trace.hit(2); Trace.argumentAction.run(); return Trace.input;
    }
    private static long number() {
        Trace.hit(2); Trace.argumentAction.run(); return Trace.number;
    }
    public static void apply(int route, Method method, MethodHandle handle) throws Throwable {
        if (route == 0) receiver().apply(argument());
        else if (route == 1) ((Base) receiver()).apply(argument());
        else {
            Owner.Child selected = receiver(); Payload value = argument();
            if (route == 2) {
                try { method.invoke(selected, value); }
                catch (InvocationTargetException failure) { throw failure.getCause(); }
            } else handle.invokeExact(selected, value);
        }
    }
    public static long wide(int route, Method method, MethodHandle handle) throws Throwable {
        if (route == 0) return receiver().wide(number());
        if (route == 1) return ((Base) receiver()).wide(number());
        Owner.Child selected = receiver(); long value = number();
        if (route == 2) {
            try { return (Long) method.invoke(selected, value); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (long) handle.invokeExact(selected, value);
    }
}
