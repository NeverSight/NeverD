import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ErasedSuperConstructorHarness {
    static int mode;
    static RuntimeException original;
    static Child callbackReceiver;
    static void require(boolean ok) { if (!ok) throw new AssertionError("erased superclass constructor contract"); }
    static final class Receiver implements Sink<Child> {
        public void take(Child value) {
            require(value.marker == 0 && (Object) value.sink == this && Trace.seen == value);
            callbackReceiver = value; Trace.step(6);
            if (mode == 2) throw original;
        }
    }
    static final class WrongReceiver implements Sink<String> {
        public void take(String value) { throw new AssertionError("missing original bridge cast"); }
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    static Child make(int route, Sink value) throws Throwable {
        if (route == 0) return new Child(value);
        if (route == 1) return ErasedSuperConstructorCaller.make(value);
        if (route == 2) {
            try { return Child.class.getConstructor(Sink.class).newInstance(value); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findConstructor(Child.class,
            MethodType.methodType(void.class, Sink.class));
        return (Child) handle.invokeExact(value);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); mode = Integer.parseInt(args[1]);
        original = new IllegalStateException("owned constructor failure");
        Trace.failure = original; Trace.failBody = mode == 3;
        Trace.initMode = mode == 5 ? 1 : mode == 6 ? 4 : 0;
        Sink<?> sink = mode == 1 ? null : mode == 4 ? new WrongReceiver() : new Receiver();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.value = 0; Trace.seen = null; callbackReceiver = null;
            Child result = null; Throwable failure = null;
            try { result = make(route, sink); } catch (Throwable caught) { failure = caught; }
            if (mode >= 5) {
                require(result == null && Trace.seen == null && callbackReceiver == null);
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == original);
                else require(failure instanceof NoClassDefFoundError);
                require(Trace.value == (attempt == 0 ? (mode == 5 ? 1 : 14) : 0));
            } else {
                int suffix = mode == 0 ? 2635 : mode == 1 ? 235 : mode == 2 ? 26 : mode == 3 ? 263 : 2;
                int factor = suffix >= 1000 ? 10000 : suffix >= 100 ? 1000 : suffix >= 10 ? 100 : 10;
                require(Trace.value == (attempt == 0 ? 14 * factor : 0) + suffix);
                require(Trace.seen instanceof Child);
                Child seen = (Child) Trace.seen;
                require((Object) seen.sink == sink);
                if (mode < 2) require(failure == null && result == seen && result.marker == 99);
                else require(result == null && seen.marker == 0);
                if (mode == 2 || mode == 3) require(failure == original);
                if (mode == 4) require(failure instanceof ClassCastException);
                require(callbackReceiver == (mode == 1 || mode == 4 ? null : seen));
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.value);
        }
    }
}
