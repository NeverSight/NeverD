import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ErasedFactoryHarness {
    static final class Callback implements Factory<Object> {
        Object value;
        public Object read() { Trace.reads++; Trace.hit(3); return value; }
    }
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("generic factory effects and erased result");
    }
    static Box<?> call(Factory<?> factory, long flags, int route) throws Throwable {
        if (route == 0) return Client.call(factory, flags);
        if (route == 1) return ErasedFactoryCaller.call(factory, flags);
        if (route == 2) {
            try { return (Box<?>) Client.class.getMethod("call", Factory.class, long.class).invoke(null, factory, flags); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Box<?>) MethodHandles.lookup().findStatic(Client.class, "call",
            MethodType.methodType(Box.class, Factory.class, long.class)).invokeExact((Factory) factory, flags);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]);
        Callback callback = new Callback();
        Object value = variant == 1 ? null : variant == 2 ? new Object() : "owned";
        callback.value = value;
        RuntimeException original = new IllegalArgumentException("owned factory failure");
        Trace.failure = original; Trace.failAt = mode;
        Box<?> previous = null;
        long events = 0;
        int reads = 0, boxes = 0;
        for (int attempt = 1; attempt <= 2; attempt++) {
            long flags = attempt == 1 ? Long.MIN_VALUE : Long.MAX_VALUE;
            int failureStage = 0;
            if (mode == 1) {
                if (attempt == 1) events = 1;
                failureStage = 1;
            } else {
                if (attempt == 1) events = 1;
                events = events * 10 + 2;
                if (mode == 2 || variant == 3) failureStage = 2;
                else {
                    events = events * 10 + 3; reads++;
                    if (mode == 3) failureStage = 3;
                    else if (mode == 4) {
                        if (attempt == 1) events = events * 10 + 4;
                        failureStage = 4;
                    } else {
                        if (attempt == 1) events = events * 10 + 4;
                        events = events * 10 + 5; boxes++;
                        if (mode == 5) failureStage = 5;
                    }
                }
            }
            try {
                Box<?> result = call(variant == 3 ? null : callback, flags, route);
                require(failureStage == 0 && result != previous && result.value == value);
                previous = result;
            } catch (Throwable error) {
                require(failureStage != 0);
                if (mode == 1 || mode == 4 && failureStage == 4) {
                    if (attempt == 1) require(error.getClass() == ExceptionInInitializerError.class && error.getCause() == original);
                    else require(error.getClass() == NoClassDefFoundError.class);
                } else if (mode == failureStage) require(error == original);
                else require(variant == 3 && error.getClass() == NullPointerException.class);
            }
            require(Trace.events == events && Trace.reads == reads && Trace.boxes == boxes);
            require(Trace.flags == (mode == 1 ? 0 : flags));
        }
        System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode + ",trace=" + Trace.events);
    }
}
