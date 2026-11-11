import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class AllocationArrayLengthHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("allocation array length changed"); }
    static Target call(Holder holder, int[] input, boolean flag, int route, int variant) throws Throwable {
        if (route == 0) {
            if (variant == 0) return Core.direct(holder, input, flag);
            if (variant == 1) return Core.field(holder, input, flag);
            if (variant == 2) return Core.live(holder, input, flag);
            return Core.nested(holder, input, flag);
        }
        if (route == 1) return AllocationArrayLengthCaller.call(variant, holder, input, flag);
        String name = new String[]{"direct", "field", "live", "nested"}[variant];
        if (route == 2) {
            try { return (Target) Core.class.getMethod(name, Holder.class, int[].class, boolean.class).invoke(null, holder, input, flag); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Target) MethodHandles.publicLookup().findStatic(Core.class, name,
            MethodType.methodType(Target.class, Holder.class, int[].class, boolean.class)).invokeExact(holder, input, flag);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        boolean nested = variant == 3, direct = variant == 0;
        Trace.failAt = mode >= 1 && mode <= 3 ? mode : mode == 6 ? 4 : mode == 7 ? 5 : 0;
        Trace.failure = new RuntimeException("owned allocation length failure");
        int[] input = mode == 4 ? null : new int[mode == 8 ? 0 : 2];
        Trace.prepared = mode == 4 ? null : new int[mode == 8 ? 0 : 5];
        Trace.replaced = mode == 9 ? null : new int[1];
        Trace.holder = mode == 5 ? null : new Holder();
        Target previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.after = -99; Trace.target = null; Trace.inner = null;
            Trace.constructors = 0; Trace.innerConstructors = 0;
            if (Trace.holder != null) Trace.holder.array = attempt == 0 ? new int[7] : Trace.prepared;
            int events = attempt == 0 ? 1 : 0;
            String failed = null;
            int failedEvent = 0;
            boolean constructedInner = false, constructedTarget = false;
            if (mode == 1) failed = attempt == 0 ? "init" : "failed-class";
            else if (mode == 4 || (!direct && mode == 5)) failed = "null";
            else {
                if (nested) {
                    if (attempt == 0) events = events * 10 + 4;
                    if (mode == 6) failed = attempt == 0 ? "init" : "failed-class";
                    else {
                        events = events * 10 + 5;
                        constructedInner = true;
                        if (mode == 7) failedEvent = 5;
                    }
                }
                if (failed == null && failedEvent == 0) {
                    events = events * 10 + 2;
                    constructedTarget = true;
                    if (mode == 2) failedEvent = 2;
                    else {
                        events = events * 10 + 3;
                        if (mode == 3) failedEvent = 3;
                    }
                }
            }
            Target result = null; Throwable failure = null;
            boolean flag = (mode & 1) != 0;
            try { result = call(Trace.holder, input, flag, route, variant); }
            catch (Throwable problem) { failure = problem; }
            require(Trace.events == events && Trace.constructors == (constructedTarget ? 1 : 0)
                && Trace.innerConstructors == (constructedInner ? 1 : 0));
            if (failedEvent != 0) require(failure == Trace.failure);
            else if ("init".equals(failed)) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
            else if ("failed-class".equals(failed)) require(failure instanceof NoClassDefFoundError);
            else if ("null".equals(failed)) require(failure instanceof NullPointerException);
            else require(failure == null && result != null && result != previous && result == Trace.target);
            require((Trace.target != null) == constructedTarget && (Trace.inner != null) == constructedInner);
            boolean completed = constructedTarget && failedEvent != 2;
            int length = mode == 8 ? 0 : direct ? 2 : 5;
            require(Trace.after == (completed ? length : -99));
            if (constructedTarget) {
                Target escaped = (Target) Trace.target;
                require(escaped.length == (completed ? length : 0)
                    && escaped.marker == (completed && nested ? Trace.inner : null)
                    && escaped.flag == (completed && flag));
                if (completed && Trace.holder != null) require(Trace.holder.array == Trace.replaced);
            }
            if (result != null) previous = result;
            System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
