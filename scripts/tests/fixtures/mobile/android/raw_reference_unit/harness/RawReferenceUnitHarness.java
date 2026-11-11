import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class RawReferenceUnitHarness {
    static void require(boolean value, String detail) { if (!value) throw new AssertionError("raw reference Unit contract: " + detail); }
    static Object call(int route, Core core, Object first, Object second, Object third) throws Throwable {
        if (route == 0) return core.invoke(first, second, third);
        if (route == 1) return ((Api) core).invoke(first, second, third);
        if (route == 2) return RawReferenceUnitCaller.call(core, first, second, third);
        if (route == 3) {
            try { return Core.class.getMethod("invoke", Object.class, Object.class, Object.class).invoke(core, first, second, third); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        if (route == 4) return (Object) MethodHandles.publicLookup().findVirtual(Core.class, "invoke",
            MethodType.methodType(Object.class, Object.class, Object.class, Object.class)).invokeExact(core, first, second, third);
        RawReferenceUnitCaller.typed(core, (Throwable) first, (Void) second, (Context) third); return null;
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean original = args[2].equals("original");
        Method bridge = Core.class.getMethod("invoke", Object.class, Object.class, Object.class);
        require(bridge.isBridge() == original && bridge.isSynthetic() == original, "reported flags");
        require(Core.class.getGenericInterfaces().length == 1 && Core.class.getGenericInterfaces()[0] == Api.class, "original raw interface");
        Method target = Core.class.getMethod("invoke", Throwable.class, Void.class, Context.class);
        require(target.getReturnType() == void.class && Modifier.isFinal(target.getModifiers()), "original typed descriptor");
        boolean initialized = false, failed = false; Object singleton = null;
        Core core = mode == 9 ? null : new Core(); Context context = new Context();
        for (int attempt = 0; attempt < 2; attempt++) {
            Object first = mode == 1 || mode == 10 ? new Object() : mode == 4 ? null : new Throwable();
            Object second = mode == 2 || mode == 10 || mode == 11 ? new Object() : null;
            Object third = mode == 3 || mode == 11 ? new Object() : mode == 5 ? null : context;
            Trace.events = Trace.calls = 0; Trace.fail = mode == 6 ? 1 : mode == 7 ? 2 : mode == 8 ? 3 : 0;
            Trace.failure = new IllegalStateException("owned body failure");
            Object sentinel = new Object(); Trace.receiver = Trace.first = Trace.second = Trace.third = sentinel;
            context.id = attempt + 41;
            int trace = 0, calls = 0; boolean stored = false, body = false;
            Class<?> expectedError = null; String cast = null;
            if (core == null) expectedError = NullPointerException.class;
            else if (mode == 1 || mode == 10) cast = "java.lang.Throwable";
            else if (mode == 2 || mode == 11) cast = "java.lang.Void";
            else if (mode == 3) cast = "fixture.Context";
            if (cast != null) expectedError = ClassCastException.class;
            if (expectedError == null) {
                calls = 1; trace = 1;
                if (mode == 6) { expectedError = IllegalStateException.class; body = true; }
                else { stored = true; trace = 12; if (mode == 7) { expectedError = IllegalStateException.class; body = true; } }
            }
            if (expectedError == null && route != 5) {
                if (failed) expectedError = NoClassDefFoundError.class;
                else if (!initialized) {
                    trace = 123; initialized = true;
                    if (mode == 8) { failed = true; expectedError = ExceptionInInitializerError.class; }
                }
            }
            Object result = null; Throwable error = null;
            try { result = call(route, core, first, second, third); } catch (Throwable caught) { error = caught; }
            require(Trace.events == trace && Trace.calls == calls, "cast/call/singleton order");
            require(expectedError == null ? error == null : error != null && error.getClass() == expectedError, "failure type");
            if (cast != null) require(error.getMessage().contains(cast), "cast precedence");
            if (body) require(error == Trace.failure, "body failure identity");
            if (expectedError == ExceptionInInitializerError.class) require(error.getCause() == Trace.failure, "initializer cause");
            require(Trace.receiver == (stored ? core : sentinel), "receiver identity");
            require(Trace.first == (stored ? first : sentinel) && Trace.second == (stored ? second : sentinel)
                && Trace.third == (stored ? third : sentinel), "argument identities and partial stores");
            require(context.id == attempt + 41, "mutable context");
            if (expectedError == null) {
                if (route == 5) require(result == null, "typed void result");
                else { require(result != null && result.getClass() == kotlin.Unit.class, "singleton result");
                    if (singleton != null) require(result == singleton, "repeated singleton identity"); singleton = result; }
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events + ",failure=" + (error == null ? "none" : error.getClass().getSimpleName()));
        }
    }
}
