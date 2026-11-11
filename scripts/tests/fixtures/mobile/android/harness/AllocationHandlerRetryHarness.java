import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class AllocationHandlerRetryHarness {
    static void require(boolean ok) {
        if (!ok) throw new AssertionError("owned allocation handler retry");
    }
    static Node call(int route, Object a, Object b, Object c, Object d,
                     Object e, Object f) throws Throwable {
        if (route == 0) return Client.make(a, b, c, d, e, f);
        if (route == 1) return AllocationHandlerGapCaller.make(a, b, c, d, e, f);
        if (route == 2) {
            try {
                return (Node) Client.class.getMethod("make", Object.class, Object.class,
                    Object.class, Object.class, Object.class, Object.class)
                    .invoke(null, a, b, c, d, e, f);
            } catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Node) MethodHandles.publicLookup().findStatic(Client.class, "make",
            MethodType.methodType(Node.class, Object.class, Object.class, Object.class,
                Object.class, Object.class, Object.class)).invokeExact(a, b, c, d, e, f);
    }
    static void arguments(Object[] actual, Object[] expected) {
        require(actual != null && actual.length == expected.length);
        for (int i = 0; i < expected.length; i++) require(actual[i] == expected[i]);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Node previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            Object a = mode == 1 ? null : new Object(), b = new Object();
            Object c = null, d = new String("owned"), e = new Object(), f = a;
            Object[] expected = {a, b, c, d, e, f};
            Trace.events = 0; Trace.partial = null; Trace.failedPartial = null;
            Trace.caught = null; Trace.arguments = null;
            Trace.failAt = mode == 0 || mode == 1 ? 0 : mode == 3 || mode == 5 ? 2 : 1;
            Trace.remaining = mode == 6 ? -1 : mode == 8 ? 2 : 1;
            Trace.failure = mode == 4 || mode == 5 ? new IllegalStateException("owned uncaught")
                                                   : new IllegalArgumentException("owned caught");
            Trace.cleanupFailure = mode == 7 ? new IllegalStateException("owned cleanup") : null;
            Node result = null; Throwable escaped = null;
            try { result = call(route, a, b, c, d, e, f); }
            catch (Throwable failure) { escaped = failure; }
            boolean caught = mode == 2 || mode == 3 || mode >= 6;
            boolean success = mode < 4 || mode == 8;
            if (success) {
                require(escaped == null && result != null && result == Trace.partial);
                require(result != previous && result != Trace.failedPartial && result.value == a);
                arguments(result.arguments, expected);
            } else require(result == null);
            if (mode == 4 || mode == 5) require(escaped == Trace.failure);
            else if (mode == 7) require(escaped == Trace.cleanupFailure);
            else require(escaped == null);
            require(Trace.caught == (caught ? Trace.failure : null));
            if (caught) {
                arguments(Trace.arguments, expected);
                Node failed = (Node) Trace.failedPartial;
                require(failed != null && failed.value == (mode == 3 ? a : null));
                if (mode == 3) arguments(failed.arguments, expected);
                else require(failed.arguments == null);
            } else require(Trace.arguments == null && Trace.failedPartial == null);
            if (mode == 4 || mode == 5) {
                Node failed = (Node) Trace.partial;
                require(failed != null && failed.value == (mode == 5 ? a : null));
                if (mode == 5) arguments(failed.arguments, expected);
                else require(failed.arguments == null);
            }
            long trace = mode == 2 ? 1312 : mode == 3 ? 12312 : mode == 4 ? 1
                       : mode == 6 ? 131313 : mode == 7 ? 13 : mode == 8 ? 131312 : 12;
            require(Trace.events == trace);
            previous = result;
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + trace);
        }
    }
}
