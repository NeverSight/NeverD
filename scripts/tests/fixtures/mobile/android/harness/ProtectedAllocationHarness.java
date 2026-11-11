import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ProtectedAllocationHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("protected allocation contract"); }
    static Object make(int variant, int route) throws Throwable {
        if (route == 0) return variant == 0 ? Maker.observed(Trace.token) : Maker.fallback();
        if (route == 1) return ProtectedAllocationCaller.make(variant);
        String name = variant == 0 ? "observed" : "fallback";
        if (route == 2) {
            try { return variant == 0 ? Maker.class.getMethod(name, Object.class).invoke(null, Trace.token)
                                      : Maker.class.getMethod(name).invoke(null); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findStatic(Maker.class, name,
            variant == 0 ? MethodType.methodType(Object.class, Object.class) : MethodType.methodType(Object.class));
        return variant == 0 ? (Object) handle.invokeExact((Object) Trace.token) : (Object) handle.invokeExact();
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        RuntimeException original = new IllegalArgumentException("owned allocation failure");
        RuntimeException fallbackFailure = new IllegalStateException("owned fallback failure");
        Trace.failure = original; Trace.fallbackFailure = fallbackFailure;
        Trace.failAt = mode == 0 ? 0 : mode == 2 ? 3 : mode == 3 ? 1 : 2;
        Trace.failFallback = mode == 4;
        int[] token = {1}; Trace.token = token;
        Object previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.value = 0; token[0]++;
            Object result = null; Throwable escaped = null;
            try { result = make(variant, route); } catch (Throwable failure) { escaped = failure; }
            if (mode == 0) {
                require(escaped == null && result instanceof Payload && ((Payload) result).value == token && result != previous);
            } else if (variant == 0) {
                require(escaped == null && result instanceof Throwable);
                if (mode == 3) {
                    if (attempt == 0) require(result instanceof ExceptionInInitializerError && ((Throwable) result).getCause() == original);
                    else require(result instanceof NoClassDefFoundError);
                } else require(result == original);
            } else if (mode == 4) {
                require(result == null);
                if (attempt == 0) require(escaped instanceof ExceptionInInitializerError && escaped.getCause() == fallbackFailure);
                else require(escaped instanceof NoClassDefFoundError);
            } else require(escaped == null && result == token);
            int trace = attempt == 0 ? 1 : 0;
            if (mode != 3) { trace = trace * 10 + 2; if (mode == 0 || mode == 2) trace = trace * 10 + 3; }
            if (mode != 0 && variant == 1 && attempt == 0) trace = trace * 10 + 4;
            require(Trace.value == trace && token[0] == attempt + 2);
            previous = result;
            System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.value);
        }
    }
}
