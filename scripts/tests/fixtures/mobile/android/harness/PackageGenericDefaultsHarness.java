import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class PackageGenericDefaultsHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("original package generic default contract");
    }
    static String call(int entry, int route, Object input, String label) throws Throwable {
        if (route == 0) {
            if (entry == 0) return Core.create(input, label);
            if (entry == 1) return Core.guarded(input, label);
            return Core.locked(input, label);
        }
        if (route == 1) return PackageGenericDefaultsCaller.run(entry, input, label);
        String name = entry == 0 ? "create" : entry == 1 ? "guarded" : "locked";
        if (route == 2) {
            try { return (String) Core.class.getMethod(name, Object.class, String.class).invoke(null, input, label); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (String) MethodHandles.lookup().findStatic(Core.class, name,
            MethodType.methodType(String.class, Object.class, String.class)).invokeExact(input, label);
    }
    public static void main(String[] args) throws Throwable {
        int entry = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        RuntimeException original = new IllegalArgumentException("owned package read failure");
        RuntimeException cleanup = new IllegalStateException("owned package cleanup failure");
        Trace.failure = original; Trace.cleanupFailure = cleanup; Trace.replacement = new Object();
        Trace.failAt = mode == 3 || mode == 5 ? 1 : mode == 4 ? 2 : mode == 6 || mode == 10 ? 5 : mode == 7 ? 4 : 0;
        Trace.cleanupAtThree = mode == 5;
        long events = 0;
        Object held = Trace.lock;
        for (int attempt = 0; attempt < 2; attempt++) {
            String value = new String("owned result"), fallback = new String("owned fallback"), label = new String("owned label");
            Object initial = mode == 1 ? null : mode == 2 || mode == 10 ? Integer.valueOf(42) : value;
            Object[] array = {initial};
            Object input = mode == 8 ? null : array;
            String name = mode == 9 ? null : label;
            Trace.fallback = fallback; Trace.last = null; Trace.result = null; Trace.complete = false;
            if (attempt == 0) events = 4;
            if (mode != 7) {
                events = events * 10 + 1;
                if (mode != 3 && mode != 5 && mode != 8 && mode != 9) events = events * 10 + 2;
                if (mode == 3 || mode == 4 || mode == 5) events = events * 10 + 3;
                events = events * 10 + 5;
            }
            Throwable error = null;
            String result = null;
            try { result = call(entry, route, input, name); }
            catch (Throwable caught) { error = caught; }
            boolean failed = mode == 2 || mode >= 5;
            if (entry == 1 && failed) { require(error == null); error = Trace.last; }
            if (!failed) {
                require(error == null && result == (mode == 0 ? value : mode == 1 ? fallback : null));
                require(Trace.last == (mode == 3 || mode == 4 ? original : null));
            } else {
                require(error != null && result == null);
                if (mode == 2) require(error.getClass() == ClassCastException.class);
                else if (mode == 5 || mode == 6 || mode == 10) require(error == cleanup);
                else if (mode == 8 || mode == 9) require(error.getClass() == NullPointerException.class);
                else if (attempt == 0) require(error.getClass() == ExceptionInInitializerError.class && error.getCause() == original);
                else require(error.getClass() == NoClassDefFoundError.class);
            }
            require(Trace.events == events && Trace.lock == held && !Thread.holdsLock(held));
            require(Trace.held == (entry == 2));
            boolean complete = mode == 0 || mode == 1 || mode == 2 || mode == 6 || mode == 10;
            require(Trace.complete == complete && Trace.result == (complete ? initial : null));
            if (mode != 7) require(Trace.input == input && Trace.label == name);
            require(array[0] == (mode == 3 || mode == 5 || mode == 7 || mode == 8 || mode == 9 ? initial : Trace.replacement));
            System.out.println("entry=" + entry + ",route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
