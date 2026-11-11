import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class IntersectionVoidHarness {
    static final class FirstOnly implements First {
        public void first() { Trace.firstCalls++; Trace.hit(4); }
    }
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("intersection call effects and original cast position");
    }
    static void call(Host host, int route) throws Throwable {
        if (route == 0) { host.run(); return; }
        if (route == 1) { IntersectionVoidCaller.run(host); return; }
        if (route == 2) {
            try { Host.class.getMethod("run").invoke(host); return; }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        MethodHandles.lookup().findVirtual(Host.class, "run", MethodType.methodType(void.class)).invokeExact(host);
    }
    static void raw(First value) throws Throwable {
        try { Provider.class.getMethod("enable", First.class).invoke(null, value); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]);
        Host host = new Host();
        RuntimeException original = new IllegalArgumentException("owned intersection failure");
        Trace.failure = original; Trace.failAt = mode;
        long events = 0;
        int first = 0, second = 0;
        for (int attempt = 1; attempt <= 2; attempt++) {
            int failureStage = 0;
            if (mode == 9) {
                if (attempt == 1) events = 9;
                failureStage = 9;
            } else {
                if (attempt == 1) events = 9;
                events = events * 10 + 1;
                if (mode == 1 || variant == 2) failureStage = 1;
                else {
                    events = events * 10 + 4; first++;
                    if (mode == 4) failureStage = 4;
                    else {
                        events = events * 10 + 2;
                        if (mode == 2 || variant == 1 || variant == 3) failureStage = 2;
                        else {
                            events = events * 10 + 5; second++;
                            if (mode == 5) failureStage = 5;
                            else {
                                events = events * 10 + 3;
                                if (mode == 3) failureStage = 3;
                            }
                        }
                    }
                }
            }
            try {
                if (variant == 1) raw(new FirstOnly());
                else if (variant == 2) raw(null);
                else call(host, route);
                require(failureStage == 0);
            } catch (Throwable error) {
                require(failureStage != 0);
                if (mode == 9) {
                    if (attempt == 1) require(error.getClass() == ExceptionInInitializerError.class && error.getCause() == original);
                    else require(error.getClass() == NoClassDefFoundError.class);
                } else if (mode == failureStage) require(error == original);
                else require(error.getClass() == (variant == 2 ? NullPointerException.class : ClassCastException.class));
            }
            require(Trace.events == events && Trace.firstCalls == first && Trace.secondCalls == second);
        }
        System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode + ",trace=" + Trace.events);
    }
}
