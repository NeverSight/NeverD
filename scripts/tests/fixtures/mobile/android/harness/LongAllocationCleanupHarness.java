import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class LongAllocationCleanupHarness {
    static void require(boolean ok) {
        if (!ok) throw new AssertionError("owned long allocation cleanup");
    }
    static Node call(int route, Object[] values) throws Throwable {
        if (route == 0) return Client.make(values[0], values[1], values[2], values[3], values[4], values[5]);
        if (route == 1) return AllocationHandlerGapCaller.make(values[0], values[1], values[2], values[3], values[4], values[5]);
        if (route == 2) {
            try {
                return (Node) Client.class.getMethod("make", Object.class, Object.class,
                    Object.class, Object.class, Object.class, Object.class).invoke(null, values);
            } catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Node) MethodHandles.publicLookup().findStatic(Client.class, "make",
            MethodType.methodType(Node.class, Object.class, Object.class, Object.class,
                Object.class, Object.class, Object.class)).invokeExact(
                    values[0], values[1], values[2], values[3], values[4], values[5]);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) {
            Object value = mode == 1 || mode == 9 || mode == 10 ? null : new Object();
            Object[] values = {value, new Object(), null, new String("owned"), new Object(), value};
            boolean caught = mode == 2 || mode == 3 || mode >= 6;
            Trace.partial = null; Trace.caught = null; Trace.events = 0; Trace.arguments = null;
            Trace.failAt = mode == 2 || mode == 4 || mode == 6 ? 1 : mode >= 3 ? 2 : 0;
            Trace.failure = mode == 4 || mode == 5 ? new IllegalStateException("owned uncaught")
                : new IllegalArgumentException("owned caught");
            Trace.cleanupCount = 0; Trace.cleanupTrace = 0; Trace.finished = 0;
            Trace.cleanupFirst = values[0]; Trace.cleanupSecond = values[1];
            Trace.cleanupFailAt = mode == 6 ? 0 : mode == 7 ? 127
                : mode == 8 || mode == 10 || mode == 11 ? 159 : -1;
            Trace.cleanupFailure = new IllegalStateException("owned cleanup failure");
            Trace.cleanupError = mode == 11 ? new AssertionError("owned cleanup error") : null;
            Node actual = null; Throwable failure = null;
            try { actual = call(route, values); } catch (Throwable problem) { failure = problem; }
            Node partial = (Node) Trace.partial;
            require(partial != null && partial.value == (Trace.failAt == 1 ? null : value));
            if (caught) {
                Throwable expected = Trace.cleanupFailAt < 0 ? null
                    : Trace.cleanupError != null ? Trace.cleanupError : Trace.cleanupFailure;
                require(actual == null && failure == expected && Trace.caught == Trace.failure);
                require(Trace.arguments != null && Trace.finished == 1);
                for (int i = 0; i < values.length; i++) require(Trace.arguments[i] == values[i]);
            } else if (mode == 4 || mode == 5) {
                require(actual == null && failure == Trace.failure && Trace.caught == null);
            } else require(failure == null && actual == partial && Trace.caught == null);
            if (!caught) require(Trace.arguments == null && Trace.finished == 0);
            if (Trace.failAt == 1) require(partial.arguments == null);
            else {
                require(partial.arguments != null);
                for (int i = 0; i < values.length; i++) require(partial.arguments[i] == values[i]);
            }
            int count = !caught ? 0 : Trace.cleanupFailAt < 0 ? 160 : Trace.cleanupFailAt + 1;
            long trace = 0;
            for (int i = 0; i < count; i++) trace = trace * 31 + i;
            require(Trace.cleanupCount == count && Trace.cleanupTrace == trace);
            long expectedEvents = caught ? Trace.failAt == 1 ? 13 : 123 : mode == 4 ? 1 : 12;
            require(Trace.events == expectedEvents);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + expectedEvents);
        }
    }
}
