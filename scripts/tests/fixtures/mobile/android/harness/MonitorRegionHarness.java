import fixture.*;
import java.util.concurrent.atomic.AtomicReference;
public final class MonitorRegionHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("monitor region contract"); }
    static void update(int route, Gate<?> gate, Object lock, Runnable callback) {
        if (route == 0) gate.update(lock, callback); else MonitorRegionCaller.update(gate, lock, callback);
    }
    static void released(Object lock) throws Exception {
        final boolean[] entered = {false};
        Thread next = new Thread(() -> { synchronized (lock) { entered[0] = true; } });
        next.setDaemon(true); next.start(); next.join(2000);
        require(!next.isAlive() && entered[0]);
    }
    public static void main(String[] args) throws Exception {
        for (int route = 0; route < 2; route++) {
            final int path = route;
            Object lock = new Object(); Gate<Object> gate = new Gate<>();
            int[] callbacks = {0};
            Runnable callback = () -> { require(Thread.holdsLock(lock)); callbacks[0]++; };
            update(route, gate, lock, callback);
            require(gate.count == 1 && callbacks[0] == 1 && gate.payload.anchor == lock);
            Payload originalPayload = gate.payload;
            update(route, gate, lock, () -> { require(Thread.holdsLock(lock)); update(path, gate, lock, callback); });
            require(gate.count == 3 && callbacks[0] == 2 && gate.payload == originalPayload);
            try { update(route, gate, null, callback); throw new AssertionError("missing null monitor failure"); }
            catch (NullPointerException expected) { require(gate.count == 3 && callbacks[0] == 2); }
            RuntimeException original = new IllegalArgumentException("owned callback failure");
            try { update(route, gate, lock, () -> { require(Thread.holdsLock(lock)); throw original; }); throw new AssertionError("missing callback failure"); }
            catch (RuntimeException failure) { require(failure == original && gate.count == 3); }
            released(lock);
            gate.failure = original;
            try { update(route, gate, lock, callback); throw new AssertionError("missing body failure"); }
            catch (RuntimeException failure) { require(failure == original && gate.count == 4 && callbacks[0] == 3); }
            released(lock); gate.failure = null;
            Gate<Object> shared = new Gate<>(); Object sharedLock = new Object();
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread[] workers = new Thread[6];
            for (int i = 0; i < workers.length; i++) {
                workers[i] = new Thread(() -> {
                    try { for (int n = 0; n < 500; n++) update(path, shared, sharedLock, () -> { require(Thread.holdsLock(sharedLock)); Thread.yield(); }); }
                    catch (Throwable problem) { failure.compareAndSet(null, problem); }
                });
                workers[i].setDaemon(true); workers[i].start();
            }
            for (Thread worker : workers) { worker.join(5000); require(!worker.isAlive()); }
            require(failure.get() == null && shared.count == 3000 && shared.payload.anchor == sharedLock);
            released(sharedLock);
        }
        System.out.println("concurrent-updates=6000,reentrant-updates=4,null-lock-failures=2,ordered-failures=4,release-checks=6");
    }
}
