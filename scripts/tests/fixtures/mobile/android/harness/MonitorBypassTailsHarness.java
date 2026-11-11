import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class MonitorBypassTailsHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("original monitor bypass tail contract");
    }
    static void call(int route, int state, int width, int height) throws Throwable {
        if (route == 0) { Core.update(state, width, height); return; }
        if (route == 1) { MonitorBypassTailsCaller.run(state, width, height); return; }
        if (route == 2) {
            try { Core.class.getMethod("update", int.class, int.class, int.class).invoke(null, state, width, height); return; }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        MethodHandles.lookup().findStatic(Core.class, "update", MethodType.methodType(void.class, int.class, int.class, int.class)).invokeExact(state, width, height);
    }
    public static void main(String[] args) throws Throwable {
        int config = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        int state = config == 0 ? 0 : config == 1 || config == 7 ? 1 : config == 3 ? 3 : 2;
        int width = config == 4 ? 0 : config == 6 ? Integer.MAX_VALUE : 7;
        int height = config == 5 ? 0 : config == 6 ? 1 : 11;
        RuntimeException original = new IllegalArgumentException("owned monitor tail failure");
        Trace.failure = original; Trace.mode = mode;
        if (config == 7 || config == 8) Trace.lock = null;
        long events = 0;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.entryLock = Trace.lock;
            Object entry = Trace.entryLock;
            Trace.capacityHeld = Trace.insideHeld = Trace.afterHeld = false;
            Trace.width = Trace.height = 0;
            Throwable expected = null;
            boolean inside = false, after = false;
            if (attempt == 0) events = 6;
            if (mode == 6) expected = original;
            else if (state != 0) {
                if (state != 1 && width > 0 && height > 0) {
                    events = events * 10 + 2;
                    if (mode == 2) expected = original;
                    else if (width + height > 0) {
                        if (entry == null) expected = new NullPointerException();
                        else {
                            inside = true; events = events * 10 + 3;
                            if (mode == 3) expected = original;
                            else if (state == 3) { events = events * 10 + 4; if (mode == 4) expected = original; }
                        }
                    }
                }
                boolean normal = state == 1 || width > 0 && height > 0 && width + height > 0;
                if (normal && expected == null) { after = true; events = events * 10 + 5; if (mode == 5) expected = original; }
            }
            Throwable actual = null;
            try { call(route, state, width, height); }
            catch (Throwable caught) { actual = caught; }
            if (mode == 6) {
                require(actual != null);
                if (attempt == 0) require(actual.getClass() == ExceptionInInitializerError.class && actual.getCause() == original);
                else require(actual.getClass() == NoClassDefFoundError.class);
            } else if (expected == original) require(actual == original);
            else if (expected != null) require(actual != null && actual.getClass() == expected.getClass());
            else require(actual == null);
            require(Trace.events == events && !Trace.capacityHeld && !Trace.afterHeld);
            require(Trace.insideHeld == inside);
            require(Trace.width == (inside ? width : 0) && Trace.height == (inside ? height : 0));
            require(entry == null || !Thread.holdsLock(entry));
            if (mode == 11 && inside) require(Trace.lock != entry);
            else require(Trace.lock == entry);
            System.out.println("entry=" + config + ",route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
