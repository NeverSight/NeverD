package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class ArrayObjectContextHarness {
    private static void require(boolean ok) {
        if (!ok) throw new AssertionError("owned array object context mismatch");
    }
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure) throws X { throw (X) failure; }
    private static int call(int route, int operation, Object values, Object context, int offset, long limit, double scale, boolean enabled) throws Throwable {
        if (route == 0) {
            if (operation == 0) return Core.call((Object[]) values, context, offset);
            if (operation == 1) return Core.wide((Object[]) values, context, limit, scale, enabled);
            return Core.checked(values, context, offset);
        }
        if (route == 1) {
            if (operation == 0) return ArrayObjectContextCaller.call((Object[]) values, context, offset);
            if (operation == 1) return ArrayObjectContextCaller.wide((Object[]) values, context, limit, scale, enabled);
            return ArrayObjectContextCaller.checked(values, context, offset);
        }
        if (route == 2) {
            try {
                if (operation == 0) return ((Integer) Core.class.getMethod("call", Object[].class, Object.class, int.class).invoke(null, values, context, offset)).intValue();
                if (operation == 1) return ((Integer) Core.class.getMethod("wide", Object[].class, Object.class, long.class, double.class, boolean.class).invoke(null, values, context, limit, scale, enabled)).intValue();
                return ((Integer) Core.class.getMethod("checked", Object.class, Object.class, int.class).invoke(null, values, context, offset)).intValue();
            } catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (operation == 0) return (int) MethodHandles.publicLookup().findStatic(Core.class, "call",
                MethodType.methodType(int.class, Object[].class, Object.class, int.class)).invokeExact((Object[]) values, context, offset);
        if (operation == 1) return (int) MethodHandles.publicLookup().findStatic(Core.class, "wide",
                MethodType.methodType(int.class, Object[].class, Object.class, long.class, double.class, boolean.class)).invokeExact((Object[]) values, context, limit, scale, enabled);
        return (int) MethodHandles.publicLookup().findStatic(Core.class, "checked",
                MethodType.methodType(int.class, Object.class, Object.class, int.class)).invokeExact(values, context, offset);
    }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        for (int attempt = 0; attempt < 2; ++attempt) {
            final Object sentinel = new Object(), payload = new Object();
            final Object context = mode == 3 ? null : new Object() { public String toString() { throw new AssertionError("owned unexpected context traversal"); } };
            final int offset = attempt == 0 ? Integer.MIN_VALUE + 53 : Integer.MAX_VALUE - 61;
            final long limit = attempt == 0 ? Long.MIN_VALUE + 71 : Long.MAX_VALUE - 83;
            final long bits = attempt == 0 ? 0x7ff8000000000042L : 0x8000000000000000L;
            final double scale = Double.longBitsToDouble(bits); final boolean enabled = attempt != 0;
            final int desired = mode == 13 ? 0 : mode == 14 ? -17 : attempt == 0 ? Integer.MAX_VALUE - 19 : Integer.MIN_VALUE + 29;
            final Object[] values = mode == 1 ? null : mode == 2 ? new Object[0] : mode == 7 ? new String[]{"owned initial"} : new Object[]{sentinel};
            final Object input = mode == 8 && operation == 2 ? new Object() : values;
            final Object boxed = mode == 9 ? null : mode == 10 ? new Object() : Integer.valueOf(desired);
            final Object[] alternate = new Object[]{new Object()};
            final Throwable expected = mode == 4 ? new IllegalStateException("owned runtime") : mode == 5 ? new AssertionError("owned error") : new IOException("owned checked");
            final int[] calls = {0};
            Trace.events = 0; Trace.values = alternate; Trace.context = sentinel; Trace.result = sentinel;
            Trace.offset = 0; Trace.limit = 0; Trace.scale = 0; Trace.enabled = false;
            Trace.action = new Runnable() { public void run() {
                require(calls[0]++ == 0 && Trace.events == 1 && Trace.values == values && Trace.context == context && Trace.result == sentinel);
                require(Trace.offset == (operation == 1 ? 0 : offset) && Trace.limit == (operation == 1 ? limit : 0));
                require(Double.doubleToRawLongBits(Trace.scale) == (operation == 1 ? bits : 0) && Trace.enabled == (operation == 1 && enabled));
                Trace.result = boxed;
                if (mode >= 4 && mode <= 6) ArrayObjectContextHarness.<RuntimeException>fail(expected);
                if (values != null && values.length != 0) values[0] = payload;
                if (mode == 12) { Trace.values = alternate; Trace.context = sentinel; Trace.offset = 0; Trace.limit = 0; Trace.scale = 0; Trace.enabled = false; }
            } };
            int result = 37; Throwable caught = null;
            try { result = call(route, operation, input, context, offset, limit, scale, enabled); } catch (Throwable failure) { caught = failure; }
            boolean badCast = mode == 8 && operation == 2;
            if (badCast) require(caught != null && caught.getClass() == ClassCastException.class && calls[0] == 0 && Trace.events == 0 && result == 37);
            else {
                require(calls[0] == 1);
                if (mode >= 4 && mode <= 6) require(caught == expected && Trace.events == 1 && result == 37);
                else if (mode == 7) require(caught != null && caught.getClass() == ArrayStoreException.class && Trace.events == 1 && result == 37 && values[0].equals("owned initial"));
                else if (mode == 9 || mode == 10) require(caught != null && caught.getClass() == (mode == 9 ? NullPointerException.class : ClassCastException.class) && Trace.events == 12 && result == 37);
                else require(caught == null && Trace.events == 12 && result == desired);
                require(Trace.result == boxed && Trace.values == (mode == 12 ? alternate : values) && Trace.context == (mode == 12 ? sentinel : context));
                if (mode < 4 || mode > 7) if (values != null && values.length != 0) require(values[0] == payload);
            }
        }
        System.out.println("attempts=2,array-object-context=matched");
    }
}
