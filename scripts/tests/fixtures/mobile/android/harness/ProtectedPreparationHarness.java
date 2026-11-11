import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ProtectedPreparationHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("protected preparation contract"); }
    static Object call(int variant, int route, Object input) throws Throwable {
        if (route == 0) return variant == 0 ? Maker.create(input) : Maker.raised(input);
        if (route == 1) return variant == 0 ? ProtectedPreparationCaller.create(input) : ProtectedPreparationCaller.raised(input);
        String name = variant == 0 ? "create" : "raised";
        if (route == 2) {
            try { return Maker.class.getMethod(name, Object.class).invoke(null, input); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Object) MethodHandles.publicLookup().findStatic(Maker.class, name,
            MethodType.methodType(Object.class, Object.class)).invokeExact(input);
    }
    static void payload(Problem p, int variant) {
        require(p.getMessage().equals(variant == 0 ? "owned create" : "owned raised"));
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Trace.failure = new IllegalArgumentException("owned constructor failure");
        Trace.handlerFailure = new IllegalStateException("owned handler failure");
        Trace.failAt = mode == 4 ? 2 : mode; Trace.failHandler = mode == 4;
        int[] mutable = {1};
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.last = null; Trace.tag = null; Trace.published = null;
            Object input = attempt == 0 ? mutable : null; mutable[0]++;
            Object result = null; Throwable escaped = null;
            try { result = call(variant, route, input); } catch (Throwable caught) { escaped = caught; }
            int expected = attempt == 0 ? 1 : 0;
            if (mode != 1) {
                expected = expected * 10 + 2;
                if (mode == 0 || mode == 3) expected = expected * 10 + 3;
            }
            if (mode != 0 || variant == 1) expected = expected * 10 + 4;
            require(Trace.events == expected);
            if (mode == 4) require(escaped == Trace.handlerFailure && result == null && Trace.last == null && Trace.tag == null);
            else {
                require(escaped == null);
                if (mode == 0) { require(result instanceof Problem && result == Trace.published); payload((Problem) result, variant); }
                else if (mode == 1) {
                    if (attempt == 0) require(result instanceof ExceptionInInitializerError && ((Throwable) result).getCause() == Trace.failure);
                    else require(result instanceof NoClassDefFoundError);
                } else require(result == Trace.failure);
                if (mode != 0 || variant == 1) require(Trace.last == result && Trace.tag == input);
                else require(Trace.last == null && Trace.tag == null);
            }
            require((Trace.published != null) == (mode == 0 || mode == 3));
            if (Trace.published != null) payload(Trace.published, variant);
            System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
