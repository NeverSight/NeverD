import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class AllocationFieldPrefixHarness {
    static final class Token implements Value {}
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("allocation field/getter order");
    }
    static Target call(Creator creator, int route) throws Throwable {
        if (route == 0) return creator.make();
        if (route == 1) return AllocationFieldPrefixCaller.make(creator);
        if (route == 2) {
            try { return (Target) Creator.class.getMethod("make").invoke(creator); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Target) MethodHandles.lookup().findVirtual(Creator.class, "make",
            MethodType.methodType(Target.class)).invokeExact(creator);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]);
        Creator creator = new Creator();
        Input first = new Input(), second = new Input();
        Value token = new Token();
        first.owner = creator; first.next = variant == 5 ? null : second;
        first.value = variant == 3 ? new Object() : variant == 4 ? null : token;
        second.value = new Token(); second.owner = creator;
        Trace.replace = variant == 1 || variant == 5;
        RuntimeException original = new IllegalArgumentException("owned allocation failure");
        Trace.failure = original; Trace.failAt = mode;
        Target previous = null;
        long expectedEvents = 0;
        int expectedReads = 0, expectedInners = 0, expectedTargets = 0;
        for (int attempt = 1; attempt <= 2; attempt++) {
            creator.input = variant == 2 ? null : first;
            int failureStage = 0;
            if (mode == 1) {
                if (attempt == 1) expectedEvents = 1;
                failureStage = 1;
            } else {
                if (attempt == 1) expectedEvents = 1;
                expectedEvents = expectedEvents * 10 + 2; expectedReads++;
                if (mode == 2 || variant == 2 || variant == 3) failureStage = 2;
                else if (mode == 3) {
                    if (attempt == 1) expectedEvents = expectedEvents * 10 + 3;
                    failureStage = 3;
                } else {
                    if (attempt == 1) expectedEvents = expectedEvents * 10 + 3;
                    expectedEvents = expectedEvents * 10 + 4; expectedInners++;
                    if (mode == 4) failureStage = 4;
                    else {
                        expectedEvents = expectedEvents * 10 + 5; expectedTargets++;
                        if (mode == 5) failureStage = 5;
                    }
                }
            }
            try {
                Target result = call(creator, route);
                require(failureStage == 0 && result != previous && result.inner != null);
                require(result.value == (variant == 4 ? null : token));
                require(result.inner.input == (variant == 1 ? second : variant == 5 ? null : first));
                if (previous != null) require(result.inner != previous.inner);
                previous = result;
            } catch (Throwable error) {
                require(failureStage != 0);
                if (mode == 1 || mode == 3 && failureStage == 3) {
                    if (attempt == 1) require(error.getClass() == ExceptionInInitializerError.class && error.getCause() == original);
                    else require(error.getClass() == NoClassDefFoundError.class);
                } else if (mode == failureStage) require(error == original);
                else require(error.getClass() == (variant == 2 ? NullPointerException.class : ClassCastException.class));
            }
            require(Trace.events == expectedEvents && Trace.reads == expectedReads);
            require(Trace.inners == expectedInners && Trace.targets == expectedTargets);
            if (variant == 1 || variant == 5) {
                boolean changed = failureStage == 0 || failureStage >= 3 || variant == 3 && mode != 2;
                require(creator.input == (changed ? first.next : first));
            }
        }
        System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode + ",trace=" + Trace.events);
    }
}
