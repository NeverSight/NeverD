import fixture.*;
import java.lang.invoke.*;
import java.lang.management.*;
import java.lang.reflect.Modifier;
import java.util.concurrent.*;

public final class TotalMonitorLeafHarness {
    static final MethodHandle OWN, READ, SUM;
    static final ThreadMXBean THREADS = ManagementFactory.getThreadMXBean();
    static {
        try {
            MethodHandles.Lookup lookup = MethodHandles.lookup();
            OWN = lookup.findVirtual(Core.class, "own", MethodType.methodType(Object.class));
            READ = lookup.findStatic(Core.class, "read", MethodType.methodType(Object.class, Object.class, Object.class));
            SUM = lookup.findStatic(Core.class, "sum", MethodType.methodType(int.class, Object.class, int.class, int.class));
        } catch (Exception failure) { throw new AssertionError(failure); }
    }
    static void check(boolean ok) {
        if (!ok) throw new AssertionError("owned total monitor leaf mismatch");
    }
    static boolean own(int mode) { return mode == 0 || mode == 1 || mode == 17; }
    static boolean sum(int mode) { return mode == 9 || mode == 10 || mode == 11 || mode == 15; }
    static Object invoke(int route, int mode, Core core, Object lock,
                         Object value, int left, int right) throws Throwable {
        if (own(mode)) {
            if (route == 2) return (Object)OWN.invokeExact(core);
            return route == 1 ? (Base)core.own() : core.own();
        }
        if (sum(mode))
            return Integer.valueOf(route == 2 ? (int)SUM.invokeExact(lock, left, right)
                                             : Core.sum(lock, left, right));
        if (route == 2) return (Object)READ.invokeExact(lock, value);
        return route == 1 ? Core.typed(lock, value) : Core.read(lock, value);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        check(Modifier.isSynchronized(Core.class.getDeclaredMethod("own").getModifiers()));
        check(Modifier.isSynchronized(Core.class.getDeclaredMethod("input", Object.class).getModifiers()));
        check(Modifier.isSynchronized(Core.class.getDeclaredMethod("total", int.class, int.class).getModifiers()));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Object token = mode == 1 ? null : new Object();
            Core core = new Core(token);
            Object value = mode == 4 ? new String("owned raw value")
                : mode == 6 ? null : mode == 7 ? new Object()
                : mode == 8 ? new Object[]{token} : mode == 12 ? new int[]{1,2}
                : mode == 3 || mode == 13 ? core : mode == 14 ? Long.valueOf(19)
                : mode == 16 ? new Base(token) {} : new Base(token);
            Object lock = mode == 5 || mode == 10 ? null : mode == 3 ? value : new Core(new Object());
            int left = mode == 9 ? Integer.MAX_VALUE : mode == 11 ? Integer.MIN_VALUE
                : mode == 15 ? 0 : 1;
            int right = mode == 11 ? -1 : mode == 15 ? 0 : 1;
            Worker worker = new Worker(route, mode, core, lock, value, left, right);
            // Initialize invocation paths before the contended observation.
            Core.read(new Core(new Object()), new Base(new Object())); Core.sum(new Core(new Object()), 0, 0); core.own();
            if (lock == null && !own(mode)) {
                worker.run();
            } else {
                Object held = own(mode) ? core : lock;
                synchronized (held) {
                    worker.start();
                    check(worker.ready.await(3, TimeUnit.SECONDS));
                    boolean blocked = false;
                    long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
                    while (!worker.finished && System.nanoTime() < deadline) {
                        ThreadInfo info = THREADS.getThreadInfo(worker.getId());
                        LockInfo monitor = info == null ? null : info.getLockInfo();
                        if (info != null && info.getThreadState() == Thread.State.BLOCKED
                            && info.getLockOwnerId() == Thread.currentThread().getId()
                            && monitor != null
                            && monitor.getIdentityHashCode() == System.identityHashCode(held)) {
                            blocked = true; break;
                        }
                        Thread.yield();
                    }
                    check(blocked);
                }
                worker.join(3000);
                check(!worker.isAlive() && worker.finished);
            }
            if (mode == 5 || mode == 10)
                check(worker.failure != null && worker.failure.getClass() == NullPointerException.class);
            else if (route == 1 && (mode == 4 || mode == 7 || mode == 8
                                   || mode == 12 || mode == 14))
                check(worker.failure != null && worker.failure.getClass() == ClassCastException.class);
            else {
                check(worker.failure == null);
                if (own(mode)) check(worker.result == core);
                else if (sum(mode)) check(((Integer)worker.result).intValue() == left + right);
                else check(worker.result == value);
            }
            check(core.value == token);
        }
        System.out.println("attempts=2,total-monitor-leaf=matched");
    }
    public static final class Worker extends Thread {
        final int route, mode, left, right;
        final Core core;
        final Object lock, value;
        final CountDownLatch ready = new CountDownLatch(1);
        volatile boolean finished;
        Object result;
        Throwable failure;
        Worker(int route, int mode, Core core, Object lock, Object value, int left, int right) {
            this.route=route; this.mode=mode; this.core=core; this.lock=lock;
            this.value=value; this.left=left; this.right=right;
            setDaemon(true);
        }
        @Override public void run() {
            ready.countDown();
            try { result=invoke(route, mode, core, lock, value, left, right); }
            catch (Throwable caught) { failure=caught; }
            finally { finished=true; }
        }
    }
}
