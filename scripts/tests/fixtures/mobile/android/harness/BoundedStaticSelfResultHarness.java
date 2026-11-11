import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
import java.io.Serializable;
public final class BoundedStaticSelfResultHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned bounded static self result");
    }
    static final class Trap extends Bound {
        public boolean equals(Object other) { throw new AssertionError("owned bounded static self result eager equals"); }
        public int hashCode() { throw new AssertionError("owned bounded static self result eager hash"); }
        public String toString() { throw new AssertionError("owned bounded static self result eager string"); }
    }
    static final class Reply extends Factory<Bound> implements Serializable {}
    static Object call(int route, int operation, Bound seed) throws Throwable {
        if (route == 0) return operation == 0 ? Client.call(seed) : Client.checked(seed);
        if (route == 1) return BoundedStaticSelfResultCaller.call(operation, seed);
        String name = operation == 0 ? "call" : "checked";
        if (route == 2) {
            try { return Client.class.getMethod(name, Bound.class).invoke(null, seed); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(operation == 0 ? Factory.class : Serializable.class, Bound.class));
        return operation == 0 ? (Factory)handle.invokeExact(seed) : (Serializable)handle.invokeExact(seed);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        final Bound seed = mode == 5 ? null : mode == 6 ? new Bound() : new Trap();
        final Object expected = mode == 7 ? null : mode == 9 ? new Factory<Bound>()
            : mode == 10 ? new Object() : mode == 11 ? new Parent.Base<Bound>() {} : new Reply();
        final RuntimeException runtimeFailure = new RuntimeException("owned callback failure");
        final Error errorFailure = new Error("owned callback error");
        Trace.failure = new RuntimeException("owned stage failure");
        Trace.failAt = mode == 1 ? 1 : mode == 2 ? 2 : mode == 8 ? 3 : 0;
        Trace.action = new Runnable() { public void run() {
            require(Trace.input == seed && Trace.events == 12);
            Trace.hit(4); Trace.result = expected;
            if (mode == 3) throw runtimeFailure;
            if (mode == 4) throw errorFailure;
        }};
        for (int attempt = 0; attempt < 2; ++attempt) {
            Object sentinel = new Object();
            Trace.events = 0; Trace.input = sentinel; Trace.result = seed;
            Throwable failure = null; Object actual = sentinel;
            try { actual = call(route, operation, seed); } catch (Throwable error) { failure = error; }
            require(Trace.events == (mode == 1 ? 1 : mode == 2 ? 12 : mode == 3 || mode == 4 ? 124 : 1243));
            require(Trace.input == (mode == 1 ? sentinel : seed));
            require(Trace.result == (mode == 1 || mode == 2 ? seed : expected));
            Throwable wanted = mode == 1 || mode == 2 || mode == 8 ? Trace.failure
                : mode == 3 ? runtimeFailure : mode == 4 ? errorFailure : null;
            boolean castFailure = mode == 10 || mode == 11 || operation == 1 && mode == 9;
            if (castFailure) require(failure instanceof ClassCastException);
            else require(failure == wanted);
            if (wanted == null && !castFailure) require(actual == expected);
            else require(actual == sentinel);
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
