import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ObjectStaticSuperHarness {
    public static final class Child extends Host {
        public Object ping() { Trace.overrides++; Trace.hit(5); return null; }
    }
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("original superclass result, dispatch and receiver");
    }
    static Object call(Host host, int route, String accessor) throws Throwable {
        if (route == 0) return host.callBase$main();
        if (route == 1) return ObjectStaticSuperCaller.run(host);
        if (route == 2) {
            try { return Host.class.getMethod(accessor, Host.class).invoke(null, host); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Object) MethodHandles.lookup().findStatic(Host.class, accessor,
                MethodType.methodType(Object.class, Host.class)).invokeExact(host);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]), resultCase = Integer.parseInt(args[4]);
        RuntimeException original = new IllegalArgumentException("owned superclass failure");
        Object first = resultCase == 0 ? new Object() : null;
        Object second = resultCase == 0 ? null : new Object();
        Trace.failure = original; Trace.failAt = mode;
        Trace.initializedResult = first; Trace.replacement = new Object();
        long events = 0;
        int calls = 0;
        for (int attempt = 1; attempt <= 2; attempt++) {
            Object expected = attempt == 1 ? first : second;
            Trace.result = attempt == 1 ? new Object() : second;
            if (attempt == 1) {
                events = 8;
                if (mode != 8) events = 87;
            }
            boolean initializerFailure = mode == 8 || mode == 7;
            if (!initializerFailure && variant != 2) { events = events * 10 + 2; calls++; }
            Host host = null;
            try {
                if (variant != 2) host = variant == 0 ? new Host() : new Child();
                Object actual = call(host, route, args[3]);
                require(!initializerFailure && variant != 2 && mode == 0);
                require(actual == expected && Trace.result == Trace.replacement);
            } catch (Throwable error) {
                if (initializerFailure) {
                    if (attempt == 1) require(error.getClass() == ExceptionInInitializerError.class && error.getCause() == original);
                    else require(error.getClass() == NoClassDefFoundError.class);
                } else if (variant == 2) require(error.getClass() == NullPointerException.class);
                else require(mode == 2 && error == original);
            }
            require(Trace.events == events && Trace.calls == calls && Trace.overrides == 0);
            if (!initializerFailure) require(Trace.receiver == host);
        }
        System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode + ",trace=" + Trace.events);
    }
}
