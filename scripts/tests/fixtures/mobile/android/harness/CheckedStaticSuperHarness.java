import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.reflect.Method;
import java.lang.reflect.InvocationTargetException;
public final class CheckedStaticSuperHarness {
    static final class Derived extends Host { @Override public void ping() { Trace.event(8); } }
    static void require(boolean value) { if (!value) throw new AssertionError("check before exact superclass dispatch"); }
    static void invoke(Host receiver, int route) throws Throwable {
        if (route == 0) { Host.invoke(receiver); return; }
        if (route == 1) { CheckedStaticSuperCaller.invoke(receiver); return; }
        Method method = Host.class.getDeclaredMethod("forward", Host.class);
        require(java.lang.reflect.Modifier.isPrivate(method.getModifiers()) && !method.isSynthetic());
        method.setAccessible(true);
        if (route == 2) {
            try { method.invoke(null, receiver); return; }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        MethodHandles.lookup().unreflect(method).invokeExact(receiver);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Host receiver = variant == 0 ? new Host() : variant == 1 ? new Derived() : null;
        RuntimeException original = new IllegalArgumentException("owned failure"), nullFailure = new IllegalStateException("owned null check");
        Trace.failure = original; Trace.nullFailure = nullFailure; Trace.failAt = mode; Trace.allowNull = mode == 6;
        long events = 0;
        Object sentinel = new Object(); Trace.checked = Trace.last = sentinel;
        for (int attempt = 1; attempt <= 2; attempt++) {
            int failed;
            if (attempt == 1) events = 1;
            if (mode == 1) failed = 1;
            else {
                events = events * 10 + 2;
                if (mode == 2) failed = 2;
                else if (receiver == null) failed = 3;
                else { events = events * 10 + 4; failed = mode == 4 ? 4 : 0; }
            }
            try { invoke(receiver, route); require(failed == 0); }
            catch (Throwable error) {
                require(failed != 0);
                if (failed == 1) {
                    if (attempt == 1) require(error.getClass() == ExceptionInInitializerError.class && error.getCause() == original);
                    else require(error.getClass() == NoClassDefFoundError.class);
                } else if (failed == 3) {
                    if (mode == 6) require(error.getClass() == NullPointerException.class);
                    else require(error == nullFailure);
                } else require(error == original);
            }
            require(Trace.events == events);
            require(Trace.checked == (mode == 1 ? sentinel : receiver));
            require(Trace.last == (mode == 1 || mode == 2 || receiver == null ? sentinel : receiver));
        }
        System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode + ":matched");
    }
}
