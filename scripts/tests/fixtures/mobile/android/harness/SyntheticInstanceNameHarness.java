import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class SyntheticInstanceNameHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("synthetic instance dispatch changed"); }
    static Runnable create(Core owner, Runnable action, int route) throws Throwable {
        if (route == 0) return owner.task(action);
        if (route == 1) return SyntheticInstanceNameCaller.create(owner, action);
        if (route == 2) {
            try { return (Runnable) Core.class.getMethod("task", Runnable.class).invoke(owner, action); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Runnable) MethodHandles.publicLookup().findVirtual(Core.class, "task",
            MethodType.methodType(Runnable.class, Runnable.class)).invokeExact(owner, action);
    }
    public static void main(String[] args) throws Throwable {
        int kind = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        final int mode = Integer.parseInt(args[2]);
        Runnable previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            Core owner = mode == 7 ? null : kind == 0 ? new Core() : new Child();
            final Core originalOwner = owner;
            final int[] payload = {5};
            final RuntimeException callbackFailure = new RuntimeException("owned callback failure");
            final Error callbackError = new AssertionError("owned callback error");
            Trace.failure = new RuntimeException("owned cleanup failure");
            int cleanup = kind == 0 ? 3 : 4;
            Trace.failAt = mode == 4 ? 1 : mode == 2 || mode == 6 ? cleanup : 0;
            Object before = new Object(); Trace.value = before; Trace.receiver = null; Trace.events = 0;
            Runnable action = mode == 3 ? null : new Runnable() {
                public void run() {
                    Trace.hit(2); Trace.value = payload;
                    if (mode == 1 || mode == 6) throw callbackFailure;
                    if (mode == 5) throw callbackError;
                }
            };
            Runnable work = null; Throwable failure = null;
            try {
                work = create(owner, action, route);
                require(Trace.events == 1 && Trace.receiver == null && Trace.value == before);
                require(work != null && work != previous);
                if (mode == 8) action = new Runnable() { public void run() { throw new AssertionError("wrong callback"); } };
                if (mode == 9) owner = null;
                payload[0] = 9;
                work.run();
            } catch (Throwable problem) { failure = problem; }
            int expected = mode == 7 ? 0 : mode == 4 ? 1 : mode == 3 ? 10 + cleanup : 120 + cleanup;
            require(Trace.events == expected);
            boolean invoked = mode != 7 && mode != 4;
            boolean callback = invoked && mode != 3;
            require(Trace.receiver == (invoked ? originalOwner : null));
            require(Trace.value == (callback ? payload : before));
            if (callback) require(payload[0] == 9);
            if (mode == 4 || mode == 2 || mode == 6) require(failure == Trace.failure);
            else if (mode == 1) require(failure == callbackFailure);
            else if (mode == 5) require(failure == callbackError);
            else if (mode == 3 || mode == 7) require(failure instanceof NullPointerException);
            else require(failure == null);
            require((work != null) == invoked);
            if (work != null) previous = work;
            System.out.println("kind=" + kind + ",route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
