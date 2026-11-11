import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class CapturedGenericConstructorHarness {
    static int mode;
    static RuntimeException original;
    static Child callbackReceiver;
    static Object capture;
    static long bits;
    static void require(boolean ok) { if (!ok) throw new AssertionError("captured generic constructor contract"); }
    static final class Receiver implements Sink<Child> {
        public void take(Child value) {
            require(value.marker == 0 && (Object) value.sink == this && Trace.seen == value
                    && value.capture == capture && value.arity == 2 && value.wide == bits);
            callbackReceiver = value; Trace.step(6);
            if (mode == 2) throw original;
        }
    }
    static final class WrongReceiver implements Sink<String> {
        public void take(String value) { throw new AssertionError("missing original bridge cast"); }
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    static Child make(int route, Sink value) throws Throwable {
        if (route == 0) return new Child(capture, value, bits);
        if (route == 1) return CapturedGenericConstructorCaller.make(capture, value, bits);
        if (route == 2) {
            try { return Child.class.getConstructor(Object.class, Sink.class, long.class).newInstance(capture, value, bits); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findConstructor(Child.class,
            MethodType.methodType(void.class, Object.class, Sink.class, long.class));
        return (Child) handle.invokeExact(capture, value, bits);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); mode = Integer.parseInt(args[1]);
        capture = mode == 7 ? null : new int[]{7};
        bits = (mode & 1) == 0 ? 0x7ff8000000000031L : 0x8000000000000007L;
        original = new IllegalStateException("owned constructor failure");
        Trace.failure = original; Trace.failBody = mode == 3;
        Trace.initMode = mode == 5 ? 1 : mode == 6 ? 4 : mode == 8 ? 7 : 0;
        Sink<?> sink = mode == 1 ? null : mode == 4 ? new WrongReceiver() : new Receiver();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.value = 0; Trace.seen = null; callbackReceiver = null;
            Child result = null; Throwable failure = null;
            try { result = make(route, sink); } catch (Throwable caught) { failure = caught; }
            if (mode == 8) {
                require(result == null && Trace.seen == null && callbackReceiver == null && failure == original);
                require(Trace.value == (attempt == 0 ? 147 : 7));
            } else if (mode == 5 || mode == 6) {
                require(result == null && Trace.seen == null && callbackReceiver == null);
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == original);
                else require(failure instanceof NoClassDefFoundError);
                require(Trace.value == (attempt == 0 ? (mode == 5 ? 1 : 14) : 0));
            } else {
                int suffix = mode == 0 || mode == 7 ? 72635 : mode == 1 ? 7235 : mode == 2 ? 726 : mode == 3 ? 7263 : 72;
                int factor = suffix >= 10000 ? 100000 : suffix >= 1000 ? 10000 : suffix >= 100 ? 1000 : suffix >= 10 ? 100 : 10;
                require(Trace.value == (attempt == 0 ? 14 * factor : 0) + suffix);
                require(Trace.seen instanceof Child);
                Child seen = (Child) Trace.seen;
                require((Object) seen.sink == sink && seen.capture == capture && seen.arity == 2 && seen.wide == bits);
                if (mode < 2 || mode == 7) require(failure == null && result == seen && result.marker == 99);
                else require(result == null && seen.marker == 0);
                if (mode == 2 || mode == 3) require(failure == original);
                if (mode == 4) require(failure instanceof ClassCastException);
                require(callbackReceiver == (mode == 1 || mode == 4 ? null : seen));
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.value);
        }
    }
}
