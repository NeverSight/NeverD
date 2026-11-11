import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class AllocationStaticCallHarness {
    static final long WORD = Long.MIN_VALUE + 37;
    static final long NAN = 0xfff0000000000001L;
    static void require(boolean ok) { if (!ok) throw new AssertionError("owned allocation order"); }
    static Target<?> call(int route, boolean raw, Object input) throws Throwable {
        double bits = Double.longBitsToDouble(NAN);
        try {
            switch (route) {
            case 0: return raw ? Creator.raw(input, WORD, bits) : Creator.checked(input, WORD, bits);
            case 1: return raw ? AllocationStaticCallCaller.raw(input, WORD, bits) : AllocationStaticCallCaller.checked(input, WORD, bits);
            case 2: return (Target<?>)Creator.class.getMethod(raw ? "raw" : "checked", Object.class, long.class, double.class)
                .invoke(null, input, WORD, bits);
            default: return (Target<?>)MethodHandles.publicLookup().findStatic(Creator.class, raw ? "raw" : "checked",
                MethodType.methodType(Target.class, Object.class, long.class, double.class)).invokeExact(input, WORD, bits);
            }
        } catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int mode = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        Value item = new Value(); item.marker = 23;
        Object input = mode == 1 ? null : mode == 2 || mode == 7 ? new Object() : item;
        RuntimeException original = new IllegalStateException("owned ordered failure");
        if (mode == 3) Trace.callFailure = original;
        if (mode == 4) Trace.constructorFailure = original;
        if (mode == 5) Trace.targetFailure = original;
        if (mode == 6) Trace.providerFailure = original;
        Object sentinel = new Object(); Trace.after = sentinel;
        int successes = 0, failures = 0; Target<?> previous = null;
        for (int attempt = 1; attempt <= 2; attempt++) {
            boolean success = mode == 0 || mode == 1 || mode == 7;
            try {
                Target<?> result = call(route, mode == 7, input);
                require(success && result != null && result != previous);
                require(result.value == input && result.original == input && Trace.after == input);
                require(result.stamp == WORD && Double.doubleToRawLongBits(result.fraction) == NAN);
                if (mode == 0) { item.marker++; require(((Value)result.value).marker == 23 + attempt); }
                previous = result; successes++;
            } catch (Throwable problem) {
                require(!success);
                if (mode == 2) require(problem.getClass() == ClassCastException.class);
                else if (mode == 3 || mode == 4) require(problem == original);
                else if (attempt == 1) require(problem.getClass() == ExceptionInInitializerError.class && problem.getCause() == original);
                else require(problem.getClass() == NoClassDefFoundError.class);
                require(Trace.after == sentinel); failures++;
            }
            int trace = mode == 5 ? 1 : mode == 6 ? 12
                : mode == 2 || mode == 3 ? (attempt == 1 ? 123 : 1233)
                : (attempt == 1 ? 1234 : 123434);
            require(Trace.events == trace && Trace.calls == (mode >= 5 && mode <= 6 ? 0 : attempt));
            require(Trace.constructors == (success || mode == 4 ? attempt : 0));
            if (Trace.calls != 0)
                require(Trace.seen == input && Trace.stamp == WORD && Double.doubleToRawLongBits(Trace.fraction) == NAN);
        }
        System.out.println("mode=" + mode + ",route=" + route + ",successes=" + successes + ",failures=" + failures
            + ",calls=" + Trace.calls + ",constructors=" + Trace.constructors + ",trace=" + Trace.events);
    }
}
