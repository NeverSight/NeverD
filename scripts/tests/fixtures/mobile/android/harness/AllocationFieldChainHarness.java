import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class AllocationFieldChainHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("allocation field chain changed"); }
    static Target create(int variant, int route, Holder holder) throws Throwable {
        if (route == 0) {
            if (variant == 0) return Core.make(holder);
            if (variant == 1) return Core.live(holder);
            return Core.keepLink(holder);
        }
        if (route == 1) return AllocationFieldChainCaller.create(variant, holder);
        String name = variant == 0 ? "make" : variant == 1 ? "live" : "keepLink";
        if (route == 2) {
            try { return (Target) Core.class.getMethod(name, Holder.class).invoke(null, holder); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Target) MethodHandles.publicLookup().findStatic(Core.class, name,
            MethodType.methodType(Target.class, Holder.class)).invokeExact(holder);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        int[] failures = {0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6};
        Trace.failAt = failures[mode]; Trace.failure = new RuntimeException("owned field chain failure");
        Trace.initialNumber = mode == 1 ? Integer.MIN_VALUE : mode == 2 ? Integer.MAX_VALUE : 17;
        Trace.afterNumber = Trace.initialNumber ^ 0x5a5a5a5a;
        Trace.initial = mode == 4 ? null : new Link();
        if (Trace.initial != null) Trace.initial.value = mode == 5 ? null : new Object();
        Trace.replacement = new Link(); Trace.replacement.value = new Object();
        boolean initialized = false, failed = false;
        for (int attempt = 0; attempt < 2; attempt++) {
            Link before = mode == 4 ? null : new Link();
            if (before != null) before.value = mode == 5 ? null : new Object();
            int beforeNumber = ~Trace.initialNumber;
            Trace.holder = new Holder(); Trace.holder.link = before; Trace.holder.count = beforeNumber;
            Holder holder = mode == 3 ? null : Trace.holder;
            Object sentinel = new Object(); Link linkSentinel = new Link();
            Trace.events = 0; Trace.seen = null; Trace.after = sentinel; Trace.afterLink = linkSentinel; Trace.afterInt = 53;
            Target result = null; Throwable failure = null;
            try { result = create(variant, route, holder); }
            catch (Throwable problem) { failure = problem; }
            int trace = 0, kind = 0, number = 0;
            Link link = before, expectedLink = before;
            int expectedNumber = beforeNumber;
            Object value = null;
            boolean entered = false, fields = false, published = false;
            if (failed) kind = 3;
            else for (int stage : new int[]{1, 2, -1, 3, 4, 5, 6}) {
                if (stage > 0 && stage <= 2 && initialized) continue;
                if (stage == 2) { expectedLink = Trace.initial; expectedNumber = Trace.initialNumber; }
                if (stage == -1) {
                    if (holder == null || expectedLink == null) { kind = 4; break; }
                    link = expectedLink; value = link.value; number = expectedNumber;
                    continue;
                }
                if (stage == 3) entered = true;
                if (stage == 4) fields = true;
                if (stage == 5) { expectedLink = Trace.replacement; expectedNumber = Trace.afterNumber; }
                if (stage == 6) published = true;
                trace = trace * 10 + stage;
                if (Trace.failAt == stage) {
                    kind = stage <= 2 ? 2 : 1;
                    if (stage <= 2) failed = true;
                    break;
                }
                if (stage == 2) initialized = true;
            }
            require(Trace.events == trace && Trace.holder.link == expectedLink && Trace.holder.count == expectedNumber);
            require(Trace.after == (published ? value : sentinel) && Trace.afterInt == (published ? number : 53));
            require(Trace.afterLink == (published && variant == 2 ? link : linkSentinel));
            if (entered) require(Trace.seen != null && Trace.seen.value == (fields ? value : null)
                && Trace.seen.number == (fields ? number : 0));
            else require(Trace.seen == null);
            if (kind == 1) require(failure == Trace.failure);
            else if (kind == 2) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
            else if (kind == 3) require(failure instanceof NoClassDefFoundError);
            else if (kind == 4) require(failure instanceof NullPointerException);
            else require(failure == null && result == Trace.seen && published);
            if (kind != 0) require(result == null);
            System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
