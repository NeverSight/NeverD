import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class AllocationFieldBeforeHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("field read before inner allocation and initialization");
    }
    static void make(Creator creator, int route) throws Throwable {
        if (route == 0) { creator.make(); return; }
        if (route == 1) { AllocationFieldBeforeCaller.make(creator); return; }
        if (route == 2) {
            try { Creator.class.getMethod("make").invoke(creator); return; }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        MethodHandles.lookup().findVirtual(Creator.class, "make", MethodType.methodType(void.class)).invokeExact(creator);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Creator creator = new Creator();
        Holder holder = new Holder();
        Object initial = variant == 1 ? null : new Object(), replacement = new Object();
        holder.value = initial;
        Trace.holder = variant == 2 ? null : holder;
        Trace.replacement = replacement;
        RuntimeException original = new IllegalArgumentException("owned allocation failure");
        Trace.failure = original; Trace.failAt = mode;
        long events = 0;
        Outer previous = null;
        for (int attempt = 1; attempt <= 2; attempt++) {
            int failed = 0;
            if (attempt == 1) events = 1;
            if (mode == 1 || variant == 2) failed = 1;
            else {
                if (attempt == 1) events = events * 10 + 3;
                if (mode == 3) failed = 3;
                else {
                    events = events * 10 + 4;
                    if (mode == 4) failed = 4;
                    else { events = events * 10 + 5; if (mode == 5) failed = 5; }
                }
            }
            try {
                make(creator, route);
                require(failed == 0 && Trace.result != null && Trace.result != previous);
                require(Trace.result.inner != null && (previous == null || Trace.result.inner != previous.inner));
                require(Trace.result.inner.value == (attempt == 1 ? initial : replacement));
                previous = Trace.result;
            } catch (Throwable error) {
                require(failed != 0);
                if (mode == 1 || mode == 3) {
                    if (attempt == 1) require(error.getClass() == ExceptionInInitializerError.class && error.getCause() == original);
                    else require(error.getClass() == NoClassDefFoundError.class);
                } else if (mode == failed) require(error == original);
                else require(variant == 2 && error.getClass() == NullPointerException.class);
                require(Trace.result == previous);
            }
            require(Trace.events == events);
            require(holder.value == (mode == 1 || mode == 3 || variant == 2 ? initial : replacement));
        }
        System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode + ",trace=" + Trace.events);
    }
}
