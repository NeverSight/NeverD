import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ConditionalAllocationHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("conditional allocation contract"); }
    static Target make(int route, Creator creator) throws Throwable {
        try {
            if (route == 0) return creator.make();
            if (route == 1) return ConditionalAllocationCaller.make(creator);
            if (route == 2) return (Target) Creator.class.getMethod("make").invoke(creator);
            return (Target) MethodHandles.publicLookup().findVirtual(Creator.class, "make",
                MethodType.methodType(Target.class)).invokeExact(creator);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Trace.failAt = mode; Trace.failure = new IllegalArgumentException("owned conditional allocation failure");
        Creator creator = new Creator(); Provider provider = new Provider();
        creator.provider = variant == 4 ? null : provider;
        provider.owner = variant == 3 ? new Object() : variant == 5 ? null : provider;
        Object first = new int[] {11}, second = new int[] {29};
        provider.first = variant == 1 ? null : new Item(first);
        provider.second = variant == 2 ? null : new Item(variant == 6 ? null : second);
        int[] sequence = variant == 1 ? new int[] {1,2,3,4,7}
            : variant == 2 ? new int[] {1,2,3,4,5}
            : variant == 3 ? new int[] {1,2,3}
            : variant == 4 ? new int[] {1} : new int[] {1,2,3,4,5,6,7};
        long expected = 0; Target previous = null;
        for (int attempt = 1; attempt <= 2; attempt++) {
            provider.calls = 0;
            provider.bits = attempt == 1 ? Long.MIN_VALUE : Long.MAX_VALUE;
            Throwable caught = null;
            try {
                Target result = make(route, creator);
                require(mode == 0 && variant != 2 && variant != 3 && variant != 4
                        && result != previous && result.bits == provider.bits
                        && result.owner == provider.owner && result.value == (variant == 1 || variant == 6 ? null : second));
                previous = result;
            } catch (Throwable failure) { caught = failure; }
            if (mode == 1) {
                if (attempt == 1) require(caught instanceof ExceptionInInitializerError && caught.getCause() == Trace.failure);
                else require(caught instanceof NoClassDefFoundError);
            } else if (mode != 0) require(caught == Trace.failure);
            else if (variant == 2 || variant == 4) require(caught instanceof NullPointerException);
            else if (variant == 3) require(caught instanceof ClassCastException);
            else require(caught == null);
            int calls = 0;
            if (attempt == 1 || mode != 1)
                for (int event : sequence) {
                    if (attempt == 2 && event == 1) continue;
                    expected = expected * 10 + event;
                    if (event == 4 || event == 5) calls++;
                    if (event == mode) break;
                }
            require(Trace.events == expected && provider.calls == calls);
        }
        System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode + ",trace=" + Trace.events);
    }
}
