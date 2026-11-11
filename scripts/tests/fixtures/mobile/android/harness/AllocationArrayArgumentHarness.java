import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class AllocationArrayArgumentHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("allocation array argument changed"); }
    static Target create(int variant, int route, Holder holder, int size, Object[] outside, int index, int tag) throws Throwable {
        if (route == 0) {
            if (variant == 0) return Core.literal(holder, size, outside, index, tag);
            if (variant == 1) return Core.sized(holder, size, outside, index, tag);
            return Core.mutate(holder, size, outside, index, tag);
        }
        if (route == 1) return AllocationArrayArgumentCaller.create(variant, holder, size, outside, index, tag);
        String name = variant == 0 ? "literal" : variant == 1 ? "sized" : "mutate";
        if (route == 2) {
            try { return (Target) Core.class.getMethod(name, Holder.class, int.class, Object[].class, int.class, int.class)
                .invoke(null, holder, size, outside, index, tag); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Target) MethodHandles.publicLookup().findStatic(Core.class, name,
            MethodType.methodType(Target.class, Holder.class, int.class, Object[].class, int.class, int.class))
            .invokeExact(holder, size, outside, index, tag);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        int[] failures = {0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 0, 0, 8};
        Trace.failAt = failures[mode]; Trace.failure = new RuntimeException("owned array preparation failure");
        Trace.initialValue = mode == 14 ? null : new String("initialized");
        Trace.afterValue = new String("after construction");
        boolean initialized = false, failed = false;
        int size = mode == 2 ? -1 : mode == 3 ? 0 : 2, index = mode == 15 ? 2 : 0, tag = Integer.MIN_VALUE;
        for (int attempt = 0; attempt < 2; attempt++) {
            String before = mode == 14 ? null : new String("before initialization");
            Trace.holder = new Holder(); Trace.holder.value = before;
            Holder holder = mode == 1 ? null : Trace.holder;
            Object sentinel = new Object();
            Object[] outside = mode == 4 ? null : mode == 5 ? new Object[0]
                : mode == 6 ? new Integer[]{null} : new Object[]{sentinel};
            Trace.events = 0; Trace.seen = null; Trace.first = null; Trace.second = null;
            Trace.alias = null; Trace.firstValue = null; Trace.readValue = null; Trace.number = 0;
            Target result = null; Throwable failure = null;
            try { result = create(variant, route, holder, size, outside, index, tag); }
            catch (Throwable problem) { failure = problem; }
            long trace = 0; int kind = 0;
            String expectedHolder = before, label = null;
            boolean entered = false, fields = false, mutated = false, published = false, outsideStored = false;
            int[] stages = variant == 1 ? new int[]{1, 2, -1, 4, 5, 6, 7, 8} : new int[]{1, 2, 3, -2, 5, 6, 7, 8};
            if (failed) kind = 3;
            else for (int stage : stages) {
                if (stage <= 2 && stage > 0 && initialized) continue;
                if (stage == 2) expectedHolder = Trace.initialValue;
                if (stage == -1) {
                    if (size < 0) { kind = 5; break; }
                    continue;
                }
                if (stage == -2) {
                    if (holder == null) { kind = 4; break; }
                    label = expectedHolder;
                    if (variant == 2) {
                        if (outside == null) { kind = 4; break; }
                        if (index >= outside.length) { kind = 6; break; }
                        if (label != null && !outside.getClass().getComponentType().isInstance(label)) { kind = 7; break; }
                        outsideStored = true;
                    }
                    continue;
                }
                if (stage == 5) entered = true;
                if (stage == 6) fields = true;
                if (stage == 7) mutated = true;
                if (stage == 8) published = true;
                trace = trace * 17 + stage;
                if (Trace.failAt == stage) {
                    kind = stage <= 2 ? 2 : 1;
                    if (stage <= 2) failed = true;
                    break;
                }
                if (stage == 2) initialized = true;
            }
            require(Trace.events == trace && Trace.holder.value == expectedHolder && Trace.readValue == label);
            if (entered) {
                int length = variant == 1 ? size : 1;
                require(Trace.seen != null && Trace.first != null && Trace.first.length == length
                    && Trace.second != null && Trace.second.length == 1 && Trace.second[0] == (variant == 1 ? tag + 1 : tag)
                    && Trace.number == (variant == 1 ? tag + 1 : tag) && Trace.firstValue == (variant == 1 ? null : label));
                if (length > 0) require(Trace.first[0] == (mutated ? Trace.afterValue : variant == 1 ? null : label));
                require(Trace.seen.left == (fields ? Trace.first : null) && Trace.seen.right == (fields ? Trace.second : null));
                require(Trace.alias == (published ? Trace.first : null));
            } else require(Trace.seen == null && Trace.first == null && Trace.second == null && Trace.alias == null);
            if (outside != null && outside.length > 0)
                require(outside[0] == (outsideStored ? label : mode == 6 ? null : sentinel));
            if (kind == 1) require(failure == Trace.failure);
            else if (kind == 2) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
            else if (kind == 3) require(failure instanceof NoClassDefFoundError);
            else if (kind == 4) require(failure instanceof NullPointerException);
            else if (kind == 5) require(failure instanceof NegativeArraySizeException);
            else if (kind == 6) require(failure instanceof ArrayIndexOutOfBoundsException);
            else if (kind == 7) require(failure instanceof ArrayStoreException);
            else require(failure == null && result == Trace.seen && published);
            if (kind != 0) require(result == null);
            System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
