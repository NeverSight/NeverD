import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class InterfaceSeedVisitorCaller {
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void call(int route, Node start, Fn operation) throws Throwable {
        if (route == 0) { Client.call(start, operation); return; }
        if (route == 1) { Provider.forward(start, operation); return; }
        if (route == 2) {
            try { Provider.class.getDeclaredMethod("forward", Node.class, Fn.class).invoke(null, start, operation); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
            return;
        }
        MethodHandle handle = MethodHandles.publicLookup().findStatic(Provider.class, "forward",
            MethodType.methodType(void.class, Node.class, Fn.class));
        handle.invokeExact(start, operation);
    }
}
