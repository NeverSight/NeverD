import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class MonitorPostReleaseHarness {
    static final Method METHOD;
    static final MethodHandle HANDLE;
    static {
        try {
            METHOD = Gate.class.getMethod("update", Object.class, Runnable.class, Runnable.class, boolean.class);
            HANDLE = MethodHandles.publicLookup().findVirtual(Gate.class, "update",
                MethodType.methodType(int.class, Object.class, Runnable.class, Runnable.class, boolean.class));
        } catch (Throwable failure) { throw new ExceptionInInitializerError(failure); }
    }
    static void require(boolean ok) { if (!ok) throw new AssertionError("post-release monitor contract"); }
    static int update(int route, Gate<?> gate, Object lock, Runnable inside, Runnable after, boolean enabled) throws Throwable {
        if (route == 0) return gate.update(lock, inside, after, enabled);
        if (route == 1) return MonitorPostReleaseCaller.update(gate, lock, inside, after, enabled);
        if (route == 2) {
            try { return (Integer) METHOD.invoke(gate, lock, inside, after, enabled); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (int) HANDLE.invokeExact((Gate) gate, lock, inside, after, enabled);
    }
    static void released(Object lock) {
        final boolean[] entered = {false};
        Thread worker = new Thread(() -> { synchronized (lock) { entered[0] = true; } });
        worker.setDaemon(true); worker.start();
        try { worker.join(2000); } catch (InterruptedException failure) { throw new AssertionError(failure); }
        require(!worker.isAlive() && entered[0]);
    }
    public static void main(String[] args) throws Throwable {
        int cases = 0, failures = 0, callbacks = 0;
        for (int route = 0; route < 4; route++) {
            for (int scenario = 0; scenario < 10; scenario++) {
                final int mode = scenario;
                Gate<Object> gate = new Gate<>(); Object lock = new Object();
                RuntimeException original = new IllegalStateException("owned continuation failure");
                int[] seen = {0, 0};
                Runnable inside = () -> {
                    require(Thread.holdsLock(lock) && gate.payload.anchor == lock);
                    seen[0]++;
                    if (mode == 5) throw original;
                    if (mode == 9) require(gate.update(lock, null, null, false) == 1);
                };
                Runnable after = () -> {
                    require(!Thread.holdsLock(lock) && gate.count == (mode == 9 ? 2 : 1));
                    require(gate.payload.anchor == lock);
                    seen[1]++; released(lock);
                    if (mode == 7) throw original;
                    if (mode == 8) require(gate.update(lock, null, null, false) == 2);
                };
                if (mode == 6) gate.failure = original;
                int result = -1; Throwable observed = null;
                try { result = update(route, gate, mode == 4 ? null : lock,
                        mode == 3 ? null : inside, mode == 2 ? null : after, mode != 1); }
                catch (Throwable failure) { observed = failure; }
                if (mode == 4) require(observed instanceof NullPointerException);
                else if (mode >= 5 && mode <= 7) require(observed == original);
                else require(observed == null && result == (mode >= 8 ? 2 : 1));
                if (observed != null) failures++;
                require(gate.count == (mode == 4 || mode == 5 ? 0 : mode >= 8 ? 2 : 1));
                require(seen[0] == (mode == 3 || mode == 4 ? 0 : 1));
                require(seen[1] == (mode == 0 || mode == 3 || mode >= 7 ? 1 : 0));
                if (mode == 4) require(gate.payload == null); else require(gate.payload.anchor == lock);
                callbacks += seen[1]; released(lock); cases++;
            }
        }
        System.out.println("cases=" + cases + ",ordered-failures=" + failures + ",unlocked-callbacks=" + callbacks);
    }
}
