package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class ArrayStarContextHarness {
    private static void require(boolean ok) {
        if (!ok) throw new AssertionError("owned array star context mismatch");
    }
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure) throws X { throw (X) failure; }
    public static class Base {}
    public static final class Leaf extends Base {}
    private static Object[] call(int route, int operation, Object values, int count, Object token, long limit, double scale, boolean enabled) throws Throwable {
        if (route == 0) {
            if (operation == 0) return Core.call((Object[]) values, count, (Class<?>) token);
            if (operation == 1) return Core.wide((Object[]) values, (Class<?>) token, limit, scale, enabled);
            return Core.checked(values, count, token);
        }
        if (route == 1) {
            if (operation == 0) return ArrayStarContextCaller.call((Object[]) values, count, (Class<?>) token);
            if (operation == 1) return ArrayStarContextCaller.wide((Object[]) values, (Class<?>) token, limit, scale, enabled);
            return ArrayStarContextCaller.checked(values, count, token);
        }
        if (route == 2) {
            try {
                if (operation == 0) return (Object[]) Core.class.getMethod("call", Object[].class, int.class, Class.class).invoke(null, values, count, token);
                if (operation == 1) return (Object[]) Core.class.getMethod("wide", Object[].class, Class.class, long.class, double.class, boolean.class).invoke(null, values, token, limit, scale, enabled);
                return (Object[]) Core.class.getMethod("checked", Object.class, int.class, Object.class).invoke(null, values, count, token);
            } catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (operation == 0) return (Object[]) MethodHandles.publicLookup().findStatic(Core.class, "call",
                MethodType.methodType(Object[].class, Object[].class, int.class, Class.class)).invokeExact((Object[]) values, count, (Class<?>) token);
        if (operation == 1) return (Object[]) MethodHandles.publicLookup().findStatic(Core.class, "wide",
                MethodType.methodType(Object[].class, Object[].class, Class.class, long.class, double.class, boolean.class)).invokeExact((Object[]) values, (Class<?>) token, limit, scale, enabled);
        return (Object[]) MethodHandles.publicLookup().findStatic(Core.class, "checked",
                MethodType.methodType(Object[].class, Object.class, int.class, Object.class)).invokeExact(values, count, token);
    }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        for (int attempt = 0; attempt < 2; ++attempt) {
            final Leaf initial = new Leaf(), payload = new Leaf();
            final Base other = new Base(); final Object sentinel = new Object();
            final Object[] unchanged = new Object[]{sentinel};
            final Object[] values = mode == 1 ? null : mode == 2 ? new Leaf[0] : new Leaf[]{initial};
            final Object[] output = mode == 9 ? null : mode == 10 ? new Base[]{other} : new Leaf[]{new Leaf()};
            final Object resultValue = mode == 16 ? new Object() : output;
            final Class<?> token = mode == 3 || mode == 12 ? null : mode == 2 ? int.class : mode == 11 || mode == 15 ? String[].class : mode == 13 ? void.class : Long.class;
            final Object input = mode == 8 && operation == 2 ? new Object() : values;
            final Object actualToken = mode == 17 && operation == 2 ? new Object() : token;
            final int count = attempt == 0 ? Integer.MIN_VALUE + 53 : Integer.MAX_VALUE - 61;
            final long limit = attempt == 0 ? Long.MIN_VALUE + 71 : Long.MAX_VALUE - 83;
            final long bits = attempt == 0 ? 0x7ff8000000000042L : 0x8000000000000000L;
            final double scale = Double.longBitsToDouble(bits); final boolean enabled = attempt != 0;
            final Throwable expected = mode == 4 ? new IllegalStateException("owned runtime") : mode == 5 ? new AssertionError("owned error") : new IOException("owned checked");
            final int[] calls = {0};
            Trace.events = 0; Trace.values = unchanged; Trace.token = Object.class; Trace.result = sentinel;
            Trace.count = 0; Trace.limit = 0; Trace.scale = 0; Trace.enabled = false;
            Trace.inspect = mode != 12 && mode != 14; Trace.kind = true;
            Trace.action = new Runnable() { public void run() {
                require(calls[0]++ == 0 && Trace.events == (operation == 1 ? 4 : 1) && Trace.values == values && Trace.token == token && Trace.result == sentinel);
                require(Trace.count == (operation == 1 ? 0 : count) && Trace.limit == (operation == 1 ? limit : 0));
                require(Double.doubleToRawLongBits(Trace.scale) == (operation == 1 ? bits : 0) && Trace.enabled == (operation == 1 && enabled));
                Trace.result = resultValue;
                if (mode >= 4 && mode <= 6) ArrayStarContextHarness.<RuntimeException>fail(expected);
                if (values != null && values.length != 0) values[0] = mode == 7 ? other : payload;
                if (mode == 15) { Trace.values = unchanged; Trace.token = Object.class; Trace.count = 0; Trace.limit = 0; Trace.scale = 0; Trace.enabled = false; }
            } };
            Object[] result = unchanged; Throwable caught = null;
            try { result = call(route, operation, input, count, actualToken, limit, scale, enabled); } catch (Throwable failure) { caught = failure; }
            final boolean badCast = operation == 2 && (mode == 8 || mode == 17);
            final boolean early = mode >= 4 && mode <= 7;
            final int events = operation == 1 ? 456 : 123;
            if (badCast) require(caught != null && caught.getClass() == ClassCastException.class && calls[0] == 0 && Trace.events == 0 && result == unchanged);
            else {
                require(calls[0] == 1 && Trace.events == (early ? events / 100 : mode == 3 ? events / 10 : events));
                if (mode >= 4 && mode <= 6) require(caught == expected && result == unchanged);
                else if (mode == 7) require(caught != null && caught.getClass() == ArrayStoreException.class && result == unchanged && values[0] == initial);
                else if (mode == 3 || mode == 16) require(caught != null && caught.getClass() == (mode == 3 ? NullPointerException.class : ClassCastException.class) && result == unchanged);
                else require(caught == null && result == output);
                require(Trace.result == resultValue && Trace.values == (mode == 15 ? unchanged : values) && Trace.token == (mode == 15 ? Object.class : token));
                if (!early && values != null && values.length != 0) require(values[0] == payload);
            }
            require(Trace.kind == (!badCast && !early && mode != 3 && Trace.inspect ? token.isArray() : true));
            if (caught == null) {
                Throwable consumerFailure = null; int length = -1;
                try { length = ((Leaf[]) result).length; } catch (Throwable failure) { consumerFailure = failure; }
                if (mode == 9) require(consumerFailure != null && consumerFailure.getClass() == NullPointerException.class);
                else if (mode == 10) require(consumerFailure != null && consumerFailure.getClass() == ClassCastException.class);
                else require(consumerFailure == null && length == 1 && result != values);
            }
        }
        System.out.println("attempts=2,array-star-context=matched");
    }
}
