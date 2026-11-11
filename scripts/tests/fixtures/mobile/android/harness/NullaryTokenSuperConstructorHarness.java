import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class NullaryTokenSuperConstructorHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned nullary token super constructor");
    }
    static Child call(int route) throws Throwable {
        if (route == 0) return Child.create();
        if (route == 1) return NullaryTokenSuperConstructorCaller.call();
        Constructor<Child> constructor = Child.class.getDeclaredConstructor();
        constructor.setAccessible(true);
        if (route == 2) {
            try { return constructor.newInstance(); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.lookup().unreflectConstructor(constructor);
        return (Child)handle.invokeExact();
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        final Object result = mode == 9 ? null : new Object();
        final Class token = mode == 11 ? null : Probe.class;
        final String changedLabel = new String("owned callback label");
        final RuntimeException runtimeFailure = new RuntimeException("owned callback failure");
        final Error errorFailure = new Error("owned callback error");
        Trace.failure = new RuntimeException("owned stage failure");
        Trace.failAt = mode >= 1 && mode <= 3 ? mode : mode >= 6 && mode <= 8 ? mode - 2 : 0;
        Trace.action = new Runnable() { public void run() {
            require(Trace.events == 123 && Trace.receiver instanceof Child);
            Parent parent = (Parent)Trace.receiver;
            require(parent.token == token && parent.label == "owned token label");
            require(Trace.token == token && Trace.label == "owned token label");
            Trace.hit(7); Trace.result = result;
            if (mode == 4) throw runtimeFailure;
            if (mode == 5) throw errorFailure;
            if (mode == 10) { parent.token = Marker.class; parent.label = changedLabel; }
        }};
        for (int attempt = 0; attempt < 2; ++attempt) {
            Object sentinel = new Object();
            Trace.events = 0; Trace.receiver = sentinel; Trace.token = sentinel;
            Trace.label = sentinel; Trace.result = sentinel;
            Throwable failure = null; Child actual = null;
            try { actual = call(route); } catch (Throwable error) { failure = error; }
            long trace = mode == 11 ? 0 : mode == 1 ? 1 : mode == 2 ? 12 : mode == 3 ? 123
                : mode == 4 || mode == 5 ? 1237 : mode == 6 ? 12374 : mode == 7 ? 123745 : 1237456;
            require(Trace.events == trace);
            boolean published = mode != 1 && mode != 11;
            if (published) {
                require(Trace.receiver instanceof Child);
                Child child = (Child)Trace.receiver;
                require((Object)child.token == (mode == 10 ? Marker.class : token));
                require(child.label == (mode == 2 ? null : mode == 10 ? changedLabel : "owned token label"));
                require(child.value == (mode == 8 || mode == 0 || mode == 9 || mode == 10 ? result : null));
            } else require(Trace.receiver == sentinel);
            require(Trace.token == (published ? token : sentinel));
            require(Trace.label == (published && mode != 2 ? "owned token label" : sentinel));
            require(Trace.result == (mode >= 1 && mode <= 3 || mode == 11 ? sentinel : result));
            Throwable wanted = mode >= 1 && mode <= 3 || mode >= 6 && mode <= 8 ? Trace.failure
                : mode == 4 ? runtimeFailure : mode == 5 ? errorFailure : null;
            if (mode == 11) require(failure instanceof NoClassDefFoundError);
            else require(failure == wanted);
            if (wanted == null && mode != 11) require(actual == Trace.receiver);
            else require(actual == null);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
