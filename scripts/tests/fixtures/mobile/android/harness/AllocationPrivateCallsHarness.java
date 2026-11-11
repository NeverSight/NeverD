import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class AllocationPrivateCallsHarness {
    public static final class Child extends Core {
        public Object read(Object input, long mask) { Trace.hit(4); return null; }
        public long readLong(Object input, long mask) { Trace.hit(5); return 0; }
    }
    static void require(boolean value) {
        if (!value) throw new AssertionError("allocation private call contract");
    }
    static Target call(int entry, int route, Core receiver, Object input, long mask) throws Throwable {
        if (route == 0) {
            if (entry == 0) return Core.create(receiver, input, mask);
            if (entry == 1) return Core.guarded(receiver, input, mask);
            return Core.locked(receiver, input, mask);
        }
        if (route == 1) return AllocationPrivateCallsCaller.call(entry, receiver, input, mask);
        String name = entry == 0 ? "create" : entry == 1 ? "guarded" : "locked";
        if (route == 2) {
            try { return (Target) Core.class.getMethod(name, Core.class, Object.class, long.class).invoke(null, receiver, input, mask); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Target) MethodHandles.publicLookup().findStatic(Core.class, name,
                MethodType.methodType(Target.class, Core.class, Object.class, long.class)).invokeExact(receiver, input, mask);
    }
    public static void main(String[] args) throws Throwable {
        int entry = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Trace.lock = new Object(); Trace.expectLock = entry == 2;
        Trace.failure = new IllegalArgumentException("owned private call failure");
        Trace.failAt = mode == 1 || mode == 2 ? mode : mode == 6 ? 8 : mode == 7 ? 9 : 0;
        Core core = new Child(); Trace.core = core;
        Trace.number = 0x123456789abcdef0L; Trace.initialValue = new Object();
        int[] input = {17};
        Object argument = mode == 4 ? null : mode == 5 ? new Object() : input;
        Core receiver = mode == 3 ? null : core;
        for (int attempt = 0; attempt < 2; attempt++) {
            core.number = Long.MIN_VALUE; core.value = attempt == 0 ? new Object() : null;
            long number = attempt == 0 ? Trace.number : core.number;
            Object value = attempt == 0 ? Trace.initialValue : core.value;
            long mask = attempt == 0 ? Long.MIN_VALUE : 0x123400000000ffffL;
            Trace.events = 0; Trace.last = null; Trace.published = null; Trace.seen = null;
            int oldInput = input[0];
            Target result = null; Throwable thrown = null;
            try { result = call(entry, route, receiver, argument, mask); } catch (Throwable failure) { thrown = failure; }
            require(!Thread.holdsLock(Trace.lock));
            long expected = attempt == 0 ? 9 : 0;
            int completed = 0;
            boolean constructor = false;
            if (mode != 7 && mode != 3) {
                expected = expected * 10 + 1;
                if (mode != 1 && mode != 4 && mode != 5) {
                    completed = 1; expected = expected * 10 + 2;
                    if (mode != 2) { completed = 2; expected = expected * 10 + 8; constructor = true; }
                }
            }
            require(Trace.events == expected && input[0] == oldInput + completed);
            require(Trace.seen == (mode == 4 || mode == 5 || completed > 0 ? argument : null));
            Throwable observed = entry == 1 ? Trace.last : thrown;
            require(entry != 1 || thrown == null);
            if (mode == 0) require(observed == null && result == Trace.published);
            else {
                require(entry != 1 || result == null);
                if (mode == 7) {
                    if (attempt == 0) require(observed instanceof ExceptionInInitializerError && observed.getCause() == Trace.failure);
                    else require(observed instanceof NoClassDefFoundError);
                } else if (mode == 3 || mode == 4) require(observed instanceof NullPointerException);
                else if (mode == 5) require(observed instanceof ClassCastException);
                else require(observed == Trace.failure);
            }
            require(constructor ? Trace.published != null : Trace.published == null);
            if (constructor) require(Trace.published.value == value && Trace.published.number == number && core.value == null);
            System.out.println("entry=" + entry + ",route=" + route + ",mode=" + mode
                    + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
