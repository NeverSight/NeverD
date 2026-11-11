import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ProtectedFinalSuffixHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("original protected final suffix contract");
    }
    static Object call(int route) throws Throwable {
        if (route == 0) return Core.read();
        if (route == 1) return ProtectedFinalSuffixCaller.read();
        if (route == 2) {
            try { return Core.class.getMethod("read").invoke(null); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Object) MethodHandles.lookup().findStatic(Core.class, "read", MethodType.methodType(Object.class)).invokeExact();
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Object a = new Object(), b = new Object(), otherA = new Object(), otherB = new Object();
        Trace.first = a; Trace.second = b; Trace.replacementFirst = otherA; Trace.replacementSecond = otherB;
        Trace.mode = mode; Trace.stamp = 0x123456789abcdef0L;
        RuntimeException original = mode == 3 || mode == 4 || mode == 9 ? new IllegalStateException("owned original")
                : mode == 5 || mode == 6 || mode == 12 ? new NullPointerException("owned original")
                : new IllegalArgumentException("owned original");
        RuntimeException secondary = new IllegalArgumentException("owned recovery failure");
        Trace.failure = original; Trace.secondary = secondary;
        boolean initialized = mode == 0 || mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 11 || mode == 13;
        boolean complete = mode == 0 || mode == 10 || mode == 13;
        Object expectedFirst = complete ? a : null;
        Object expectedSecond = complete ? (mode == 13 ? otherB : b) : null;
        long expectedStamp = complete ? 0x123456789abcdef0L : 0;
        long events = mode == 7 ? 6 : mode == 1 || mode == 3 || mode == 5 ? 61
                : mode == 2 || mode == 4 || mode == 6 || mode == 8 || mode == 9 ? 612 : 6123;
        boolean caught = mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 8 || mode == 9 || mode == 11;
        if (caught) events = events * 10 + (mode == 3 || mode == 4 || mode == 9 ? 5 : 4);
        boolean reachedSuffix = initialized || mode == 10;
        if (reachedSuffix) events = events * 10 + 7;
        for (int attempt = 0; attempt < 2; attempt++) {
            if (initialized) events = events * 10 + 8;
            Object actual = null; Throwable failure = null;
            try { actual = call(route); } catch (Throwable value) { failure = value; }
            if (initialized) {
                require(failure == null && actual == expectedFirst);
                require(Core.first == expectedFirst && Core.second == expectedSecond);
                require(Core.stamp == expectedStamp && Core.complete == complete);
            } else if (attempt == 0) {
                require(failure != null && failure.getClass() == ExceptionInInitializerError.class);
                require(failure.getCause() == (mode == 8 || mode == 9 || mode == 10 ? secondary : original));
            } else require(failure != null && failure.getClass() == NoClassDefFoundError.class);
            require(Trace.events == events && Trace.caught == (caught ? original : null));
            if (reachedSuffix) require(Trace.observedFirst == expectedFirst && Trace.observedSecond == expectedSecond
                    && Trace.observedStamp == expectedStamp && Trace.observedComplete == complete);
            else require(Trace.observedFirst == null && Trace.observedSecond == null && Trace.observedStamp == 0 && !Trace.observedComplete);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
