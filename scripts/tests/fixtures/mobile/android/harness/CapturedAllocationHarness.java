import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class CapturedAllocationHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("captured allocation contract"); }
    static Owner.Child call(int variant, int route, Owner owner, Input input, Object tag) throws Throwable {
        if (route == 0) return variant == 0 ? owner.make(tag) : owner.conditional(input, tag);
        if (route == 1) return variant == 0 ? CapturedAllocationCaller.make(owner, tag) : CapturedAllocationCaller.conditional(owner, input, tag);
        if (route == 2) {
            try {
                return variant == 0
                    ? (Owner.Child) Owner.class.getMethod("make", Object.class).invoke(owner, tag)
                    : (Owner.Child) Owner.class.getMethod("conditional", Input.class, Object.class).invoke(owner, input, tag);
            } catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (variant == 0) return (Owner.Child) MethodHandles.publicLookup().findVirtual(Owner.class, "make",
            MethodType.methodType(Owner.Child.class, Object.class)).invokeExact(owner, tag);
        return (Owner.Child) MethodHandles.publicLookup().findVirtual(Owner.class, "conditional",
            MethodType.methodType(Owner.Child.class, Input.class, Object.class)).invokeExact(owner, input, tag);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        int branch = Integer.parseInt(args[2]), mode = Integer.parseInt(args[3]);
        Owner owner = new Owner(); Input input = branch == 0 ? null : new Input(); owner.input = input;
        Trace.failure = new IllegalArgumentException("owned captured failure"); Trace.failAt = mode;
        int[] mutable = {3};
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.seen = null; Trace.child = null;
            Object tag = attempt == 0 ? mutable : null; mutable[0]++;
            Object value = attempt == 0 ? owner : null;
            if (input != null) input.value = value;
            Throwable thrown = null; Owner.Child result = null;
            try { result = call(variant, route, owner, input, tag); } catch (Throwable caught) { thrown = caught; }
            int expected = 0; boolean fails = false, nullRead = false, sawOuter = false, sawChild = false;
            if (attempt == 0 || mode != 1) {
                int[] sequence = attempt == 0 ? new int[]{1,2,3,4,5,6} : new int[]{2,3,4,5,6};
                for (int event : sequence) {
                    if (event == 2 && input == null) {
                        if (variant == 0) { nullRead = true; break; }
                        continue;
                    }
                    expected = expected * 10 + event;
                    if (event == 4) sawOuter = true;
                    if (event == 6) sawChild = true;
                    if (event == mode) { fails = true; break; }
                }
            } else fails = true;
            require(Trace.events == expected);
            if (fails && mode == 1) {
                if (attempt == 0) require(thrown instanceof ExceptionInInitializerError && thrown.getCause() == Trace.failure);
                else require(thrown instanceof NoClassDefFoundError);
            } else if (fails) require(thrown == Trace.failure);
            else if (nullRead) require(thrown instanceof NullPointerException);
            else require(thrown == null && result != null && result == Trace.child);
            require(Trace.seen == (sawOuter ? owner : null) && (Trace.child != null) == sawChild);
            if (sawChild) require(Trace.child.outer() == owner && Trace.child.tag == tag
                && Trace.child.value == (input == null ? null : value));
            System.out.println("variant=" + variant + ",route=" + route + ",branch=" + branch
                + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
