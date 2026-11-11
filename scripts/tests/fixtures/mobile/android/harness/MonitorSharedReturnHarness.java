import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class MonitorSharedReturnHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("monitor shared continuation"); }
    static void update(int route, Gate<?> gate, Object lock, Runnable callback) throws Throwable {
        if (route == 0) gate.update(lock, callback);
        else if (route == 1) MonitorRegionCaller.update(gate, lock, callback);
        else if (route == 2) {
            try { Gate.class.getMethod("update", Object.class, Runnable.class).invoke(gate, lock, callback); }
            catch (InvocationTargetException e) { throw e.getCause(); }
        } else {
            MethodHandle h = MethodHandles.publicLookup().findVirtual(Gate.class, "update",
                MethodType.methodType(void.class, Object.class, Runnable.class));
            h.invokeExact(gate, lock, callback);
        }
    }
    public static void main(String[] args) throws Throwable {
        MonitorRegionHarness.main(args);
        int cases = 0;
        for (int route = 0; route < 4; route++) for (int branch = 0; branch < 2; branch++)
        for (int mode = 0; mode < 8; mode++) {
            Gate<Object> gate = new Gate<>(); gate.bypass = branch == 1;
            Object lock = mode == 4 || mode == 6 ? null : new Object();
            RuntimeException callbackFailure = new IllegalArgumentException("owned callback");
            RuntimeException bodyFailure = new UnsupportedOperationException("owned body");
            Error callbackError = new AssertionError("owned callback error");
            int[] events = {0};
            final int test = mode;
            Runnable callback = mode == 2 || mode == 6 ? null : () -> {
                require(gate.bypass || Thread.holdsLock(lock));
                require(gate.bypass || gate.payload.anchor == lock);
                if (lock != null) require(Thread.holdsLock(lock) != gate.bypass);
                events[0]++;
                if (test == 3) gate.failure = bodyFailure;
                if (test == 1) throw callbackFailure;
                if (test == 7) throw callbackError;
            };
            if (mode == 5) gate.failure = bodyFailure;
            boolean locked = !gate.bypass && callback != null;
            Throwable expected = locked && lock == null ? new NullPointerException()
                : mode == 1 ? callbackFailure : mode == 7 ? callbackError
                : locked && (mode == 3 || mode == 5) ? bodyFailure : null;
            Throwable actual = null;
            try { update(route, gate, lock, callback); } catch (Throwable problem) { actual = problem; }
            require(expected instanceof NullPointerException ? actual instanceof NullPointerException : actual == expected);
            boolean callbackReached = callback != null && (gate.bypass || lock != null);
            boolean callbackReturned = callbackReached && mode != 1 && mode != 7;
            require(events[0] == (callbackReached ? 1 : 0));
            require(gate.count == (locked && callbackReturned ? 1 : 0));
            require((gate.payload != null) == (locked && lock != null));
            if (lock != null) MonitorRegionHarness.released(lock);
            cases++;
        }
        require(cases == 64);
        System.out.println("shared-continuations=64,branch-routes=4");
    }
}
