import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class TrailingAllocationAbortCaller {
    public static Outer call(int route, float first, float second, int mode) throws Throwable {
        if (route == 0) return Maker.make(first, second, mode);
        if (route == 1) return direct(first, second, mode);
        if (route == 2) {
            try { return (Outer)Maker.class.getDeclaredMethod("make", float.class, float.class, int.class).invoke(null, first, second, mode); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findStatic(Maker.class, "make",
            MethodType.methodType(Outer.class, float.class, float.class, int.class));
        return (Outer)handle.invokeExact(first, second, mode);
    }
    static Outer direct(float first, float second, int mode) { return Maker.make(first, second, mode); }
}
