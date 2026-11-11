import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class ProtectedAllocationCleanupHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("protected allocation cleanup contract");
    }

    static Object call(Object input, boolean clean, int route) throws Throwable {
        if (route == 0) return Maker.create(input, clean);
        if (route == 1) return ProtectedAllocationCleanupCaller.call(input, clean);
        if (route == 2) {
            try { return Maker.class.getMethod("create", Object.class, boolean.class).invoke(null, input, clean); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return MethodHandles.publicLookup().findStatic(Maker.class, "create",
                MethodType.methodType(Object.class, Object.class, boolean.class)).invoke(input, clean);
    }

    public static void main(String[] args) throws Throwable {
        boolean clean = Integer.parseInt(args[0]) != 0;
        int route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Object input = mode % 2 == 0 ? new int[]{17} : null;
        Trace.constructorFailure = new IllegalArgumentException("owned constructor failure");
        Trace.handlerFailure = new IllegalStateException("owned handler failure");
        Trace.cleanupFailure = new UnsupportedOperationException("owned cleanup failure");
        Trace.failConstructor = mode == 1 || mode == 2 || mode == 3 || mode == 5;
        Trace.failRecord = mode == 2 || mode == 8;
        Trace.failInner = mode == 3 || mode == 4;
        Trace.failOuter = mode == 5 || mode == 6 || mode == 9;
        Trace.failInit = mode >= 7;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0;
            Trace.last = null;
            Trace.tag = null;
            Trace.published = null;
            Throwable thrown = null;
            Object result = null;
            try { result = call(input, clean, route); } catch (Throwable failure) { thrown = failure; }
            boolean primary = Trace.failConstructor || Trace.failInit;
            int expected = attempt == 0 ? 8 : 0;
            if (!Trace.failInit) expected = expected * 10 + 3;
            if (primary) expected = expected * 10 + 4;
            if (clean) expected = expected * 10 + 1;
            if (!(clean && Trace.failInner)) expected = expected * 10 + 2;
            require(Trace.events == expected);
            boolean recorded = primary && !Trace.failRecord;
            require(recorded ? Trace.last != null && Trace.tag == input : Trace.last == null && Trace.tag == null);
            if (recorded) {
                if (!Trace.failInit) require(Trace.last == Trace.constructorFailure);
                else if (attempt == 0) require(Trace.last instanceof ExceptionInInitializerError
                        && Trace.last.getCause() == Trace.constructorFailure);
                else require(Trace.last instanceof NoClassDefFoundError);
            }
            Throwable expectedFailure = clean && Trace.failInner || Trace.failOuter ? Trace.cleanupFailure
                    : primary && Trace.failRecord ? Trace.handlerFailure : null;
            require(thrown == expectedFailure);
            if (expectedFailure == null) {
                if (primary) require(result == Trace.last);
                else require(result == Trace.published && result instanceof Problem
                        && ((Problem) result).getMessage().equals("owned protected cleanup"));
            }
            require(primary ? Trace.published == null : Trace.published != null);
            System.out.println("clean=" + clean + ",route=" + route + ",mode=" + mode
                    + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
