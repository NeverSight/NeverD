import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class RecursivePairResultHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned recursive pair result");
    }
    static final class Trap implements Comparable<Object> {
        public int compareTo(Object other) { throw new AssertionError("owned recursive pair result eager compare"); }
        public boolean equals(Object other) { throw new AssertionError("owned recursive pair result eager equals"); }
        public int hashCode() { throw new AssertionError("owned recursive pair result eager hash"); }
        public String toString() { throw new AssertionError("owned recursive pair result eager string"); }
    }
    static Object call(int route, int operation, Comparable left, Comparable right) throws Throwable {
        if (route == 0) return operation == 0 ? Client.call(left, right) : Client.checked(left, right);
        if (route == 1) return RecursivePairResultCaller.call(operation, left, right);
        String name = operation == 0 ? "call" : "checked";
        if (route == 2) {
            try { return Client.class.getMethod(name, Comparable.class, Comparable.class).invoke(null, left, right); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(operation == 0 ? Comparable.class : String.class, Comparable.class, Comparable.class));
        return operation == 0 ? (Comparable)handle.invokeExact(left, right) : (String)handle.invokeExact(left, right);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        final Comparable left = mode == 5 || mode == 11 ? null : new Trap();
        final Comparable right = mode == 6 || mode == 11 ? null : mode == 7 ? left : new Trap();
        final Object expected = mode == 9 ? null : mode == 10 ? left : mode == 12 ? new Object()
            : mode == 13 ? right : mode == 14 ? new Trap() : new String("owned opaque reply");
        final RuntimeException runtimeFailure = new RuntimeException("owned callback failure");
        final Error errorFailure = new Error("owned callback error");
        Trace.failure = new RuntimeException("owned stage failure");
        Trace.failAt = mode == 1 ? 1 : mode == 2 ? 2 : mode == 8 ? 3 : 0;
        Trace.action = new Runnable() { public void run() {
            require(Trace.left == left && Trace.right == right && Trace.events == 12);
            Trace.hit(4); Trace.result = expected;
            if (mode == 3) throw runtimeFailure;
            if (mode == 4) throw errorFailure;
        }};
        for (int attempt = 0; attempt < 2; ++attempt) {
            Object sentinel = new Object();
            Trace.events = 0; Trace.left = Trace.right = sentinel; Trace.result = left;
            Throwable failure = null; Object actual = sentinel;
            try { actual = call(route, operation, left, right); } catch (Throwable error) { failure = error; }
            require(Trace.events == (mode == 1 ? 1 : mode == 2 ? 12 : mode == 3 || mode == 4 ? 124 : 1243));
            if (mode == 1) require(Trace.left == sentinel && Trace.right == sentinel);
            else require(Trace.left == left && Trace.right == right);
            require(Trace.result == (mode == 1 || mode == 2 ? left : expected));
            Throwable wanted = mode == 1 || mode == 2 || mode == 8 ? Trace.failure
                : mode == 3 ? runtimeFailure : mode == 4 ? errorFailure : null;
            boolean castFailure = mode == 12 || operation == 1 && (mode == 10 || mode == 13 || mode == 14);
            if (castFailure) require(failure instanceof ClassCastException);
            else require(failure == wanted);
            if (wanted == null && !castFailure) require(actual == expected);
            else require(actual == sentinel);
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
