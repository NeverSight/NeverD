import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ConditionalArrayAllocationHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("conditional array allocation contract"); }
    static Target make(int route, Creator creator) throws Throwable {
        try {
            if (route == 0) return creator.make();
            if (route == 1) return ConditionalArrayAllocationCaller.make(creator);
            if (route == 2) return (Target) Creator.class.getMethod("make").invoke(creator);
            return (Target) MethodHandles.publicLookup().findVirtual(Creator.class, "make",
                MethodType.methodType(Target.class)).invokeExact(creator);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        Trace.failAt = variant == 9 ? 1 : variant == 10 ? 2 : variant == 11 ? 3
            : variant == 12 ? 7 : variant == 13 ? 4 : 0;
        Trace.failure = new IllegalStateException("owned array argument failure");
        Provider provider = new Provider(); Creator creator = new Creator();
        Object first = new int[] {11}, second = new long[] {29}, fallback = new Object();
        provider.first = variant == 1 ? Integer.MIN_VALUE : variant == 2 ? Integer.MAX_VALUE
            : variant == 14 ? -1 : 0;
        provider.second = variant == 2 || variant == 8 ? 0 : variant == 3 ? -1
            : variant == 4 ? Integer.MAX_VALUE : 1;
        provider.fallback = variant == 14 ? null : fallback;
        provider.data = variant == 5 ? null : variant == 8 ? new Object[0]
            : new Object[] {first, variant == 7 ? null : second};
        creator.provider = variant == 6 ? null : provider;
        Object expectedValue = variant == 1 ? fallback : variant == 14 || variant == 7 ? null
            : variant == 2 ? first : second;
        int[] sequence = variant == 1 || variant == 14 ? new int[] {1,2,7}
            : variant == 6 ? new int[] {1}
            : variant == 3 || variant == 4 || variant == 5 || variant == 8 ? new int[] {1,2,3,4}
            : new int[] {1,2,3,4,7};
        long expectedTrace = 0; Target previous = null;
        for (int attempt = 1; attempt <= 2; attempt++) {
            provider.reads = provider.arrayReads = 0;
            Throwable caught = null;
            try {
                Target result = make(route, creator);
                require(Trace.failAt == 0 && variant != 3 && variant != 4 && variant != 5
                    && variant != 6 && variant != 8 && result != previous && result.selected == expectedValue);
                previous = result;
            } catch (Throwable failure) { caught = failure; }
            if (Trace.failAt == 1) {
                if (attempt == 1) require(caught instanceof ExceptionInInitializerError && caught.getCause() == Trace.failure);
                else require(caught instanceof NoClassDefFoundError);
            } else if (Trace.failAt != 0) require(caught == Trace.failure);
            else if (variant == 3 || variant == 4 || variant == 8) require(caught instanceof ArrayIndexOutOfBoundsException);
            else if (variant == 5 || variant == 6) require(caught instanceof NullPointerException);
            else require(caught == null);
            int reads = 0, arrayReads = 0;
            if (attempt == 1 || Trace.failAt != 1)
                for (int event : sequence) {
                    if (attempt == 2 && event == 1) continue;
                    expectedTrace = expectedTrace * 10 + event;
                    if (event == 2 || event == 4) reads++;
                    if (event == 3) arrayReads++;
                    if (event == Trace.failAt) break;
                }
            require(Trace.events == expectedTrace && provider.reads == reads && provider.arrayReads == arrayReads);
        }
        System.out.println("variant=" + variant + ",route=" + route + ",trace=" + Trace.events);
    }
}
