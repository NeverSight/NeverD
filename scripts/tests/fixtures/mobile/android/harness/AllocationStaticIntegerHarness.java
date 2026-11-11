import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class AllocationStaticIntegerHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("static integer allocation contract"); }
    static Target make(int route, Creator creator) throws Throwable {
        try {
            if (route == 0) return creator.make();
            if (route == 1) return AllocationStaticIntegerCaller.make(creator);
            if (route == 2) return (Target) Creator.class.getMethod("make").invoke(creator);
            return (Target) MethodHandles.publicLookup().findVirtual(Creator.class, "make",
                MethodType.methodType(Target.class)).invokeExact(creator);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        Trace.failAt = variant == 4 ? 1 : variant == 5 ? 3 : variant == 6 ? 2 : variant == 7 ? 7 : 0;
        Trace.failure = new IllegalArgumentException("owned static field failure");
        Trace.initialNumber = 19;
        Trace.afterCall = variant == 1 ? Integer.MIN_VALUE : variant == 2 ? Integer.MAX_VALUE : variant == 8 ? 0 : -29;
        Trace.afterConstructor = ~Trace.afterCall;
        Object owner = variant == 3 ? null : new int[] {11};
        Trace.replacement = new long[] {31};
        Creator creator = new Creator(); creator.provider = new Provider();
        Target previous = null; long expectedTrace = 0;
        for (int attempt = 1; attempt <= 2; attempt++) {
            Trace.owner = owner;
            Throwable caught = null;
            try {
                Target result = make(route, creator);
                require(Trace.failAt == 0 && result != previous && result.owner == owner
                    && result.number == Trace.afterCall && Fields.number == Trace.afterConstructor
                    && Trace.owner == Trace.replacement);
                previous = result;
            } catch (Throwable failure) { caught = failure; }
            if (Trace.failAt == 1 || Trace.failAt == 3) {
                if (attempt == 1) require(caught instanceof ExceptionInInitializerError && caught.getCause() == Trace.failure);
                else require(caught instanceof NoClassDefFoundError);
            } else if (Trace.failAt != 0) require(caught == Trace.failure);
            else require(caught == null);
            if (attempt == 1 || Trace.failAt != 1)
                for (int event : new int[] {1,2,3,7}) {
                    if (attempt == 2 && (event == 1 || event == 3)) continue;
                    expectedTrace = expectedTrace * 10 + event;
                    if (attempt == 2 && Trace.failAt == 3 && event == 2) break;
                    if (event == Trace.failAt) break;
                }
            require(Trace.events == expectedTrace && Trace.calls == (Trace.failAt == 1 ? 0 : attempt));
        }
        System.out.println("variant=" + variant + ",route=" + route + ",trace=" + Trace.events);
    }
}
