import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class SequentialMonitorsHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("owned sequential monitor contract"); }
    static Object invoke(int route, Object first, Object second, Object input) throws Throwable {
        try {
            if (route == 0) return Core.run(first, second, input);
            if (route == 1) return SequentialMonitorsCaller.run(first, second, input);
            if (route == 2) return Core.class.getMethod("run", Object.class, Object.class, Object.class).invoke(null, first, second, input);
            return (Object) MethodHandles.publicLookup().findStatic(Core.class, "run",
                MethodType.methodType(Object.class, Object.class, Object.class, Object.class)).invokeExact(first, second, input);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    static Object second(int route, int mask, Object first, Object second, Object input) throws Throwable {
        if ((mask & 2) != 0 && second != null) synchronized (second) { return invoke(route, first, second, input); }
        return invoke(route, first, second, input);
    }
    static Object run(int route, int mask, Object first, Object second, Object input) throws Throwable {
        if ((mask & 1) != 0 && first != null) synchronized (first) { return second(route, mask, first, second, input); }
        return second(route, mask, first, second, input);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]), mask = Integer.parseInt(args[2]);
        Object first = mode == 9 ? null : new Object();
        Object second = mode == 10 ? null : mode == 11 ? first : new Object();
        Object input = mode == 12 ? null : new Object();
        Trace.early = mode == 1 || mode == 3;
        Trace.skip = mode == 2 || mode == 3;
        Trace.failAt = mode >= 4 && mode <= 8 ? mode - 3 : 0;
        Trace.baseMask = mask;
        Trace.failure = new IllegalStateException("owned sequential failure");
        long expectedEvents = 0;
        for (int attempt = 0; attempt < 2; attempt++) {
            boolean expectedFailure = false, nullFailure = false;
            int last = Trace.early ? 2 : Trace.skip ? 3 : 5;
            for (int event = 1; event <= last; event++) {
                if ((event == 2 && first == null) || (event == 4 && second == null)) {
                    expectedFailure = nullFailure = true;
                    break;
                }
                expectedEvents = expectedEvents * 10 + event;
                if (Trace.failAt == event) { expectedFailure = true; break; }
            }
            Throwable caught = null;
            Object result = new Object();
            try { result = run(route, mask, first, second, input); }
            catch (Throwable failure) { caught = failure; }
            require(nullFailure ? caught instanceof NullPointerException
                : expectedFailure ? caught == Trace.failure : caught == null && result == input);
            require(Trace.events == expectedEvents && Trace.seen == input);
            require(first == null || !Thread.holdsLock(first));
            require(second == null || !Thread.holdsLock(second));
            System.out.println("route=" + route + ",mode=" + mode + ",mask=" + mask + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
