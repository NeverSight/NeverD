import fixture.*;
import java.util.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class RecursiveArrayCarrierResultHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned recursive array carrier result");
    }
    static final class Trap implements Comparable<Object> {
        public int compareTo(Object other) { throw new AssertionError("owned recursive array carrier result eager compare"); }
        public boolean equals(Object other) { throw new AssertionError("owned recursive array carrier result eager equals"); }
        public int hashCode() { throw new AssertionError("owned recursive array carrier result eager hash"); }
        public String toString() { throw new AssertionError("owned recursive array carrier result eager string"); }
    }
    static final class Result extends AbstractList<Object> {
        final int mode;
        final Object cell;
        final RuntimeException runtime;
        final Error error;
        Result(int mode, Object cell, RuntimeException runtime, Error error) {
            this.mode = mode; this.cell = cell; this.runtime = runtime; this.error = error;
        }
        public Object get(int index) {
            require(index == 0 && Trace.events == 1243);
            Trace.hit(5);
            if (mode == 13) throw runtime;
            if (mode == 14) throw error;
            return cell;
        }
        public int size() { throw new AssertionError("owned recursive array carrier result eager size"); }
    }
    static Object call(int route, int operation, Comparable[] values) throws Throwable {
        if (route == 0) return operation == 0 ? Client.call(values) : Client.checked(values);
        if (route == 1) return RecursiveArrayCarrierResultCaller.call(operation, values);
        String name = operation == 0 ? "call" : "checked";
        if (route == 2) {
            try { return Client.class.getMethod(name, Comparable[].class).invoke(null, (Object)values); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(operation == 0 ? List.class : String.class, Comparable[].class));
        return operation == 0 ? (List)handle.invokeExact(values) : (String)handle.invokeExact(values);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        final Comparable[] values = mode == 5 ? null : mode == 6 ? new Comparable[0]
            : mode == 7 ? new String[] {"owned component"} : new Comparable[] {new Trap(), new Trap()};
        final Comparable replacement = new Trap();
        final Object cell = mode == 10 ? new Trap() : new String("owned opaque cell");
        final RuntimeException runtimeFailure = new RuntimeException("owned callback failure");
        final Error errorFailure = new Error("owned callback error");
        final Object result = mode == 9 ? null : mode == 12 ? new Object()
            : new Result(mode, cell, runtimeFailure, errorFailure);
        Trace.failure = new RuntimeException("owned stage failure");
        Trace.failAt = mode == 1 ? 1 : mode == 2 ? 2 : mode == 8 ? 3 : mode == 15 ? 5 : 0;
        Trace.action = new Runnable() { public void run() {
            require(Trace.input == values && Trace.events == 12);
            Trace.hit(4); Trace.result = result;
            if (mode == 3) throw runtimeFailure;
            if (mode == 4) throw errorFailure;
            if (mode == 7 || mode == 11) ((Object[])values)[0] = replacement;
        }};
        for (int attempt = 0; attempt < 2; ++attempt) {
            Object sentinel = new Object();
            Trace.events = 0; Trace.input = sentinel; Trace.result = values;
            Throwable failure = null; Object actual = sentinel;
            try { actual = call(route, operation, values); } catch (Throwable error) { failure = error; }
            boolean callbackFailed = mode == 3 || mode == 4 || mode == 7;
            boolean gotCell = operation == 1 && mode != 1 && mode != 2 && !callbackFailed
                && mode != 8 && mode != 9 && mode != 12;
            require(Trace.events == (mode == 1 ? 1 : mode == 2 ? 12 : callbackFailed ? 124 : gotCell ? 12435 : 1243));
            require(Trace.input == (mode == 1 ? sentinel : values));
            require(Trace.result == (mode == 1 || mode == 2 ? values : result));
            if (mode == 7) require(values[0] instanceof String);
            if (mode == 11) require(values[0] == replacement);
            Throwable wanted = mode == 1 || mode == 2 || mode == 8 || gotCell && mode == 15 ? Trace.failure
                : mode == 3 || gotCell && mode == 13 ? runtimeFailure
                : mode == 4 || gotCell && mode == 14 ? errorFailure : null;
            boolean castFailure = mode == 12 || gotCell && mode == 10;
            boolean nullFailure = operation == 1 && mode == 9;
            if (mode == 7) require(failure instanceof ArrayStoreException);
            else if (castFailure) require(failure instanceof ClassCastException);
            else if (nullFailure) require(failure instanceof NullPointerException);
            else require(failure == wanted);
            if (wanted == null && !castFailure && !nullFailure && mode != 7)
                require(actual == (operation == 0 ? result : cell));
            else require(actual == sentinel);
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
