import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class NestedCaptureHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("nested capture order"); }
    static Bundle make(int route, Box box, Object value) throws Throwable {
        try {
            if (route == 0) return Creator.make(box, value);
            if (route == 1) return NestedCaptureCaller.make(box, value);
            if (route == 2) return (Bundle)Creator.class.getMethod("make", Box.class, Object.class).invoke(null, box, value);
            return (Bundle)MethodHandles.publicLookup().findStatic(Creator.class, "make", MethodType.methodType(Bundle.class, Box.class, Object.class)).invokeExact(box, value);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int mode = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        RuntimeException original = new IllegalArgumentException("owned capture failure");
        Trace.failAt = mode; Trace.failure = original;
        Bundle previous = null;
        for (int attempt = 1; attempt <= 2; attempt++) {
            Object value = attempt == 1 ? new Object() : null;
            Box box = attempt == 1 ? new Box() : null;
            try {
                Bundle result = make(route, box, value);
                require(mode == 0 && result != previous && result.first != result.second && result.direct == value);
                if (previous != null) require(result.first != previous.first && result.second != previous.second);
                require(result.second.read() == value);
                if (box == null) {
                    try { result.first.read(); throw new AssertionError("null capture accepted"); }
                    catch (NullPointerException expected) {}
                } else {
                    require(result.first.read() == null && box.calls == 1);
                    box.value = value;
                    require(result.first.read() == value && box.calls == 2);
                    box.failure = original;
                    try { result.first.read(); throw new AssertionError("capture failure lost"); }
                    catch (RuntimeException problem) { require(problem == original && box.calls == 3); }
                }
                previous = result;
            } catch (Throwable failure) {
                require(mode != 0);
                if (mode <= 2) {
                    if (attempt == 1) require(failure.getClass() == ExceptionInInitializerError.class && failure.getCause() == original);
                    else require(failure.getClass() == NoClassDefFoundError.class);
                } else require(failure == original);
            }
            long expected = mode == 1 ? 1 : mode == 2 ? 12 : mode == 3 ? (attempt == 1 ? 123 : 1233) : mode == 4 ? (attempt == 1 ? 1234 : 123434) : (attempt == 1 ? 12345 : 12345345);
            require(Trace.events == expected);
        }
        System.out.println("mode=" + mode + ",route=" + route + ",trace=" + Trace.events);
    }
}
