import fixture.Callback;
import fixture.Core;
import fixture.Recoverable;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class MonitorTypedContinuationHarness {
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError("owned monitor typed continuation: " + reason);
    }
    private static void raise(Throwable failure) throws Exception {
        if (failure instanceof Exception) throw (Exception)failure;
        throw (Error)failure;
    }
    private static final class Observer implements Callback {
        final int mode;
        final Object lock, seed = new Object(), result = new Object(), recovered = new Object();
        final Recoverable specific = new Recoverable("recover");
        final Exception checked = new Exception("checked"), runtime = new IllegalArgumentException("runtime");
        final Error error = new AssertionError("error");
        final Throwable secondary;
        final List<Integer> events = new ArrayList<Integer>();
        CountDownLatch acquired;
        Observer(Object lock, int mode) {
            this.lock = lock; this.mode = mode;
            secondary = mode == 11 || mode == 13 ? new AssertionError("secondary") : new Exception("secondary");
        }
        void context(int phase, Object actualLock, Object actualSeed) throws Exception {
            require(actualLock == lock && actualSeed == seed, "callback input identity");
            require(Thread.holdsLock(lock) == (phase == 1), "original monitor ownership");
            events.add(phase);
            if (phase != 1 && acquired != null)
                require(acquired.await(5, TimeUnit.SECONDS), "release before unlocked callback");
        }
        public void locked(Object actualLock, Object actualSeed) throws Exception {
            context(1, actualLock, actualSeed);
            final CountDownLatch started = new CountDownLatch(1);
            acquired = new CountDownLatch(1);
            Thread competitor = new Thread(new Runnable() {
                public void run() {
                    started.countDown();
                    synchronized (lock) { acquired.countDown(); }
                }
            });
            competitor.setDaemon(true); competitor.start();
            require(started.await(5, TimeUnit.SECONDS) && !acquired.await(15, TimeUnit.MILLISECONDS), "held monitor excludes competitor");
            if (mode >= 3 && mode <= 5) raise(mode == 3 ? specific : mode == 4 ? checked : error);
        }
        public Object unlocked(Object actualLock, Object actualSeed) throws Exception {
            context(2, actualLock, actualSeed);
            if (mode == 6 || mode == 10 || mode == 11 || mode == 14) throw specific;
            if (mode == 7 || mode == 12 || mode == 13) throw checked;
            if (mode == 8) throw runtime;
            if (mode == 9) throw error;
            return result;
        }
        public Object recover(Object actualLock, Object actualSeed, Recoverable failure) throws Exception {
            context(3, actualLock, actualSeed);
            require(failure == specific && (mode == 6 || mode == 10 || mode == 11 || mode == 14), "first matching typed handler");
            if (mode == 10 || mode == 11) raise(secondary);
            return mode == 14 ? null : recovered;
        }
        public void observe(Object actualLock, Object actualSeed, Exception failure) throws Exception {
            context(4, actualLock, actualSeed);
            require(failure == (mode == 8 ? runtime : checked) && (mode == 7 || mode == 8 || mode == 12 || mode == 13), "ordinary typed handler");
            if (mode == 12 || mode == 13) raise(secondary);
        }
        void verify(Object answer, Throwable caught) throws Exception {
            List<Integer> expected = mode == 1 || mode == 2 ? Arrays.<Integer>asList()
                    : mode >= 3 && mode <= 5 ? Arrays.asList(1)
                    : mode == 6 || mode == 10 || mode == 11 || mode == 14 ? Arrays.asList(1, 2, 3)
                    : mode == 7 || mode == 8 || mode == 12 || mode == 13 ? Arrays.asList(1, 2, 4) : Arrays.asList(1, 2);
            require(events.equals(expected), "ordered callback inventory " + events + " expected " + expected);
            if (lock != null) require(!Thread.holdsLock(lock), "release on every completion");
            if (acquired != null) require(acquired.await(5, TimeUnit.SECONDS), "release on abrupt completion");
            Throwable wanted = mode == 3 ? specific : mode == 4 || mode == 7 ? checked : mode == 5 || mode == 9 ? error
                    : mode == 8 ? runtime : mode >= 10 && mode <= 13 ? secondary : null;
            if (mode == 1 || mode == 2) require(caught != null && caught.getClass() == NullPointerException.class, "original null failure");
            else require(caught == wanted, "original exception identity and handler scope");
            Object value = mode == 0 || mode == 15 ? result : mode == 6 ? recovered : null;
            require(answer == value, "result identity or null recovery");
        }
    }
    private static Object compiledCaller(Object lock, Callback callback, Object seed) throws Exception {
        return Core.apply(lock, callback, seed);
    }
    private static Object call(int route, Object lock, Callback callback, Object seed) throws Throwable {
        if (route == 0) {
            try { return Core.class.getMethod("apply", Object.class, Callback.class, Object.class).invoke(null, lock, callback, seed); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (route == 1) {
            MethodHandle handle = MethodHandles.publicLookup().findStatic(Core.class, "apply",
                    MethodType.methodType(Object.class, Object.class, Callback.class, Object.class));
            return (Object)handle.invokeExact(lock, callback, seed);
        }
        return compiledCaller(lock, callback, seed);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Object lock = mode == 1 ? null : new Object();
        for (int attempt = 0; attempt < 2; ++attempt) for (int repeat = 0; repeat < (mode == 15 ? 2 : 1); ++repeat) {
            Observer observer = new Observer(lock, mode); Object answer = null; Throwable failure = null;
            try { answer = call(route, lock, mode == 2 ? null : observer, observer.seed); }
            catch (Throwable caught) { failure = caught; }
            observer.verify(answer, failure);
        }
        System.out.println("attempts=2,monitor-typed-continuation=matched");
    }
}
