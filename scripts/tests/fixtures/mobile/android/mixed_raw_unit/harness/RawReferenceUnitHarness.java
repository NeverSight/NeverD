import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class RawReferenceUnitHarness {
    static void require(boolean value, String detail) { if (!value) throw new AssertionError("raw reference Unit contract: " + detail); }
    static final class Probe extends Number {
        final int value;
        Probe(int value) { this.value = value; }
        public int intValue() { Trace.unboxes++; Trace.hit(4); return value; }
        public long longValue() { return value; }
        public float floatValue() { return value; }
        public double doubleValue() { return value; }
    }
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
        RawReferenceUnitCaller.typed(core, (Void) first, (Context) second, ((Number) third).intValue()); return null;
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean original = args[2].equals("original");
        Method bridge = Core.class.getMethod("invoke", Object.class, Object.class, Object.class);
        require(bridge.isBridge() == original && bridge.isSynthetic() == original, "reported flags");
        require(Core.class.getGenericInterfaces().length == 1 && Core.class.getGenericInterfaces()[0] == Api.class, "original raw interface");
        Method target = Core.class.getMethod("invoke", Void.class, Context.class, int.class);
        require(target.getReturnType() == void.class && Modifier.isFinal(target.getModifiers()), "original typed descriptor");
        boolean initialized = false, failed = false; Object singleton = null;
        Core core = mode == 9 ? null : new Core(); Context context = new Context();
        for (int attempt = 0; attempt < 2; ++attempt) {
            Object first = mode == 1 || mode == 10 ? new Object() : null;
            Object second = mode == 2 || mode == 10 || mode == 11 ? new Object() : mode == 4 ? null : context;
            int expectedMask = mode == 13 ? (int)(0x1234567887654321L + attempt) : mode == 14 || mode == 15 ? 0 :
                mode == 16 ? Integer.MAX_VALUE : mode == 17 ? -123 : 0x87654321 + attempt;
            Object third;
            if (mode == 3 || mode == 11) third = new Object();
            else if (mode == 5) third = null;
            else if (mode == 13) third = Long.valueOf(0x1234567887654321L + attempt);
            else if (mode == 14) third = Double.valueOf(Double.NaN);
            else if (mode == 15) third = Long.valueOf(Long.MIN_VALUE);
            else if (mode == 16) third = Float.valueOf(Float.POSITIVE_INFINITY);
            else if (mode == 17) third = Byte.valueOf((byte)-123);
            else third = new Probe(expectedMask);
            Trace.events = Trace.calls = Trace.unboxes = 0;
            Trace.fail = mode == 6 ? 1 : mode == 7 ? 2 : mode == 8 ? 3 : mode == 12 ? 4 : 0;
            Trace.failure = new IllegalStateException("owned mixed bridge failure");
            Object sentinel = new Object(); Trace.receiver = Trace.first = Trace.second = sentinel;
            Trace.mask = 0x76543210; context.id = attempt + 41;
            int trace = 0, calls = 0, unboxes = 0; boolean stored = false, body = false;
            Class<?> expectedError = null; String cast = null;
            if (core == null && route != 5) expectedError = NullPointerException.class;
            else if (mode == 1 || mode == 10) cast = "java.lang.Void";
            else if (mode == 2 || mode == 11) cast = "fixture.Context";
            else if (mode == 3) cast = "java.lang.Number";
            if (cast != null) expectedError = ClassCastException.class;
            if (expectedError == null) {
                if (third == null) expectedError = NullPointerException.class;
                else if (third instanceof Probe) {
                    trace = 4; unboxes = 1;
                    if (mode == 12) { expectedError = IllegalStateException.class; body = true; }
                }
            }
            if (expectedError == null && core == null) expectedError = NullPointerException.class;
            if (expectedError == null) {
                calls = 1; trace = trace * 10 + 1;
                if (mode == 6) { expectedError = IllegalStateException.class; body = true; }
                else { stored = true; trace = trace * 10 + 2; if (mode == 7) { expectedError = IllegalStateException.class; body = true; } }
            }
            if (expectedError == null && route != 5) {
                if (failed) expectedError = NoClassDefFoundError.class;
                else if (!initialized) {
                    trace = trace * 10 + 3; initialized = true;
                    if (mode == 8) { failed = true; expectedError = ExceptionInInitializerError.class; }
                }
            }
            Object result = null; Throwable error = null;
            try { result = call(route, core, first, second, third); } catch (Throwable caught) { error = caught; }
            require(Trace.events == trace && Trace.calls == calls && Trace.unboxes == unboxes, "cast/unbox/call/singleton order");
            require(expectedError == null ? error == null : error != null && error.getClass() == expectedError, "failure type");
            if (cast != null) require(error.getMessage().contains(cast), "cast precedence");
            if (body) require(error == Trace.failure, "unbox/body failure identity");
            if (expectedError == ExceptionInInitializerError.class) require(error.getCause() == Trace.failure, "initializer cause");
            require(Trace.receiver == (stored ? core : sentinel), "receiver identity");
            require(Trace.first == (stored ? first : sentinel) && Trace.second == (stored ? second : sentinel), "argument identities and partial stores");
            require(Trace.mask == (stored ? expectedMask : 0x76543210) && context.id == attempt + 41, "original scalar and mutable context");
            if (expectedError == null) {
                if (route == 5) require(result == null, "typed void result");
                else { require(result != null && result.getClass() == kotlin.Unit.class, "singleton result");
                    if (singleton != null) require(result == singleton, "repeated singleton identity"); singleton = result; }
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events + ",failure=" +
                (error == null ? "none" : error.getClass().getSimpleName()));
        }
    }
}
