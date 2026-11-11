import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class AllocationStatementHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("allocation statement order"); }
    static Target make(int route, Object payload) throws Throwable {
        try {
            if (route == 0) return Creator.make(payload);
            if (route == 1) return AllocationStatementCaller.make(payload);
            if (route == 2) return (Target)Creator.class.getMethod("make", Object.class).invoke(null, payload);
            return (Target)MethodHandles.publicLookup().findStatic(Creator.class, "make", MethodType.methodType(Target.class, Object.class)).invokeExact(payload);
        } catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int mode = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        RuntimeException original = new IllegalArgumentException("owned statement failure");
        Trace.failAt = mode; Trace.failure = original; Target previous = null;
        for (int attempt = 1; attempt <= 2; attempt++) {
            Object payload = attempt == 1 ? new Object() : null;
            try {
                Target result = make(route, payload);
                require(mode == 0 && result != previous && result.item == payload && result.type == Token.class);
                previous = result;
            } catch (Throwable failure) {
                require(mode != 0);
                if (mode <= 2) {
                    if (attempt == 1) require(failure.getClass() == ExceptionInInitializerError.class && failure.getCause() == original);
                    else require(failure.getClass() == NoClassDefFoundError.class);
                } else require(failure == original);
            }
            long expected = mode == 1 ? 1 : mode == 2 ? 12 : mode == 3 ? (attempt == 1 ? 123 : 1233) : (attempt == 1 ? 1234 : 123434);
            require(Trace.events == expected && Trace.calls == (mode == 1 || mode == 2 ? 0 : attempt));
            require(Trace.ctors == (mode == 0 || mode == 4 ? attempt : 0));
        }
        System.out.println("mode=" + mode + ",route=" + route + ",trace=" + Trace.events);
    }
}
