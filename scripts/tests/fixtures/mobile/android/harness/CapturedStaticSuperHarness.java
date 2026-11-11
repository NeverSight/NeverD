import fixture.*;
import fixture.Outer.Host;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;
public final class CapturedStaticSuperHarness {
    public static final class Child extends Host {
        public Child(Outer owner) { owner.super(); }
        public void ping() { Trace.overrides++; Trace.hit(5); }
    }
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("original captured superclass binding and receiver");
    }
    static void call(Host host, int route, String accessor) throws Throwable {
        if (route == 0) { host.callBase$main(); return; }
        if (route == 1) { CapturedStaticSuperCaller.run(host); return; }
        if (route == 2) {
            try { Host.class.getMethod(accessor, Host.class).invoke(null, host); return; }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        MethodHandles.lookup().findStatic(Host.class, accessor, MethodType.methodType(void.class, Host.class)).invokeExact(host);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]);
        boolean projected = args[4].equals("1");
        require(Host.class.isMemberClass() && Host.class.getEnclosingClass() == Outer.class);
        require(Modifier.isStatic(Host.class.getModifiers()) == projected);
        java.lang.reflect.Field capture = Host.class.getDeclaredField("this$0");
        capture.setAccessible(true);
        require(capture.getType() == Outer.class && Modifier.isFinal(capture.getModifiers()));
        require(capture.isSynthetic() != projected);
        require(Host.class.getConstructor(Outer.class).getParameterTypes()[0] == Outer.class);
        RuntimeException original = new IllegalArgumentException("owned captured superclass failure");
        Trace.failure = original; Trace.failAt = mode;
        long events = 0;
        int calls = 0;
        for (int attempt = 1; attempt <= 2; attempt++) {
            Outer owner = new Outer();
            if (attempt == 1) {
                events = 8;
                if (mode != 8) events = 87;
            }
            boolean initializerFailure = mode == 8 || mode == 7;
            if (!initializerFailure && variant != 2) { events = events * 10 + 2; calls++; }
            Host host = null;
            try {
                if (variant != 2) host = variant == 0 ? owner.new Host() : new Child(owner);
                call(host, route, args[3]);
                require(!initializerFailure && variant != 2 && mode == 0);
            } catch (Throwable error) {
                if (initializerFailure) {
                    if (attempt == 1) require(error.getClass() == ExceptionInInitializerError.class && error.getCause() == original);
                    else require(error.getClass() == NoClassDefFoundError.class);
                } else if (variant == 2) require(error.getClass() == NullPointerException.class);
                else require(mode == 2 && error == original);
            }
            require(Trace.events == events && Trace.calls == calls && Trace.overrides == 0);
            if (!initializerFailure) require(Trace.receiver == host);
            if (host != null) require(capture.get(host) == owner);
        }
        System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode + ",trace=" + Trace.events);
    }
}
