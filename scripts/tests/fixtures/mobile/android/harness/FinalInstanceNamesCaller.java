import fixture.Terminal;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class FinalInstanceNamesCaller {
    public static Object frozen(Terminal receiver, Object input) { return receiver.entry(input); }
    public static Object call(int route, String spelling, Terminal receiver, Object input) throws Throwable {
        if (route == 0) return receiver.entry(input);
        if (route == 1) return frozen(receiver, input);
        if (route == 2) {
            try { return Terminal.class.getDeclaredMethod(spelling, Object.class).invoke(receiver, input); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return MethodHandles.lookup().findVirtual(Terminal.class, spelling,
            MethodType.methodType(Object.class, Object.class)).invokeExact(receiver, input);
    }
}
