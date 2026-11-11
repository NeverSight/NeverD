import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class AllocationSuperCallHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("allocation super call changed"); }
    static Target create(Core owner, int variant, int route) throws Throwable {
        if (route == 0) return variant == 0 ? owner.make() : owner.live();
        if (route == 1) return AllocationSuperCallCaller.create(owner, variant);
        String name = variant == 0 ? "make" : "live";
        if (route == 2) {
            try { return (Target) Core.class.getMethod(name).invoke(owner); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Target) MethodHandles.publicLookup().findVirtual(Core.class, name,
            MethodType.methodType(Target.class)).invokeExact(owner);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), kind = Integer.parseInt(args[1]);
        int route = Integer.parseInt(args[2]), mode = Integer.parseInt(args[3]);
        int[] failures = {0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
        Trace.failAt = failures[mode]; Trace.failure = new RuntimeException("owned super argument failure");
        Trace.initialNumber = mode == 1 ? Integer.MIN_VALUE : mode == 2 ? Integer.MAX_VALUE : 17;
        Trace.afterNumber = Trace.initialNumber ^ 0x5a5a5a5a;
        Trace.seed = mode == 4 ? null : new Object(); Trace.replacement = new Object();
        boolean initialized = false, failed = false;
        for (int attempt = 0; attempt < 2; attempt++) {
            Core owner = mode == 3 ? null : kind == 0 ? new Core() : new Child();
            Object before = new Object(), sentinel = new Object();
            int beforeNumber = ~Trace.initialNumber;
            Trace.value = before; Trace.number = beforeNumber;
            Trace.events = 0; Trace.baseReceiver = null; Trace.seen = null;
            Trace.after = sentinel; Trace.afterInt = 53;
            Target result = null; Throwable failure = null;
            try { result = create(owner, variant, route); }
            catch (Throwable problem) { failure = problem; }
            int trace = 0, problem = 0, number = 0;
            Object expectedValue = before, value = null;
            int expectedNumber = beforeNumber;
            boolean called = false, entered = false, fields = false, published = false;
            if (owner == null) problem = 4;
            else if (failed) problem = 3;
            else for (int stage = 1; stage <= 8; stage++) {
                if (stage <= 2 && initialized) continue;
                if (stage == 2) { expectedValue = Trace.seed; expectedNumber = Trace.initialNumber; }
                if (stage == 3) { called = true; value = expectedValue; }
                if (stage == 4) number = expectedNumber;
                if (stage == 5) entered = true;
                if (stage == 6) fields = true;
                if (stage == 7) { expectedValue = Trace.replacement; expectedNumber = Trace.afterNumber; }
                if (stage == 8) published = true;
                trace = trace * 10 + stage;
                if (Trace.failAt == stage) {
                    problem = stage <= 2 ? 2 : 1;
                    if (stage <= 2) failed = true;
                    break;
                }
                if (stage == 2) initialized = true;
            }
            require(Trace.events == trace && Trace.value == expectedValue && Trace.number == expectedNumber);
            require(Trace.baseReceiver == (called ? owner : null));
            require(Trace.after == (published ? value : sentinel) && Trace.afterInt == (published ? number : 53));
            if (entered) require(Trace.seen != null && Trace.seen.owner == (fields ? owner : null)
                && Trace.seen.value == (fields ? value : null) && Trace.seen.number == (fields ? number : 0));
            else require(Trace.seen == null);
            if (problem == 1) require(failure == Trace.failure);
            else if (problem == 2) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
            else if (problem == 3) require(failure instanceof NoClassDefFoundError);
            else if (problem == 4) require(failure instanceof NullPointerException);
            else require(failure == null && result == Trace.seen && published);
            if (problem != 0) require(result == null);
            System.out.println("variant=" + variant + ",kind=" + kind + ",route=" + route + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
