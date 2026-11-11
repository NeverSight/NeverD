package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class ArrayNominalVarargsHarness {
    private static void require(boolean ok) {
        if (!ok) throw new AssertionError("owned array nominal varargs mismatch");
    }
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure) throws X { throw (X) failure; }
    private static Object call(int route, int operation, Object context, int selector, Object values) throws Throwable {
        if (route == 0) return operation == 0 ? Core.call(context, selector, (Object[]) values) : Core.checked(context, selector, values);
        if (route == 1) return operation == 0 ? ArrayNominalVarargsCaller.call(context, selector, (Object[]) values) : ArrayNominalVarargsCaller.checked(context, selector, values);
        String name = operation == 0 ? "call" : "checked";
        Class<?> array = operation == 0 ? Object[].class : Object.class;
        if (route == 2) {
            try { return Core.class.getMethod(name, Object.class, int.class, array).invoke(null, context, selector, values); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (operation == 0) return (Object) MethodHandles.publicLookup().findStatic(Core.class, name,
                MethodType.methodType(Object.class, Object.class, int.class, Object[].class)).asFixedArity().invokeExact(context, selector, (Object[]) values);
        return (Object) MethodHandles.publicLookup().findStatic(Core.class, name,
                MethodType.methodType(Object.class, Object.class, int.class, Object.class)).invokeExact(context, selector, values);
    }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        require(Core.class.getMethod("call", Object.class, int.class, Object[].class).isVarArgs());
        require(Provider.class.getMethod("forward", Object.class, int.class, Object[].class).isVarArgs());
        for (int attempt = 0; attempt < 2; ++attempt) {
            final Object sentinel = new Object(), payload = new Object();
            final Object context = mode == 3 ? null : new Object() { public String toString() { throw new AssertionError("owned unexpected context traversal"); } };
            final int selector = attempt == 0 ? Integer.MIN_VALUE + 17 : Integer.MAX_VALUE - 31;
            final Object[] values = mode == 1 ? null : mode == 2 ? new Object[0] : mode == 7 ? new String[]{"owned initial"} : new Object[]{sentinel};
            final Object input = mode == 8 && operation == 1 ? new Object() : values;
            final Object desired = mode == 9 ? null : mode == 10 ? values : mode == 11 ? context : payload;
            final Object[] alternate = new Object[]{new Object()};
            final Throwable expected = mode == 4 ? new IllegalStateException("owned runtime") : mode == 5 ? new AssertionError("owned error") : new IOException("owned checked");
            final int[] calls = {0};
            Trace.events = 0; Trace.context = sentinel; Trace.selector = 0; Trace.values = alternate; Trace.result = sentinel;
            Trace.action = new Runnable() { public void run() {
                require(calls[0]++ == 0 && Trace.events == 1 && Trace.context == context && Trace.selector == selector && Trace.values == values && Trace.result == sentinel);
                Trace.result = desired;
                if (mode >= 4 && mode <= 6) ArrayNominalVarargsHarness.<RuntimeException>fail(expected);
                if (values != null && values.length != 0) values[0] = payload;
                if (mode == 12) { Trace.values = alternate; Trace.context = sentinel; Trace.selector = 0; }
            } };
            Object result = sentinel; Throwable caught = null;
            try { result = call(route, operation, context, selector, input); } catch (Throwable failure) { caught = failure; }
            boolean badCast = mode == 8 && operation == 1;
            if (badCast) require(caught != null && caught.getClass() == ClassCastException.class && calls[0] == 0 && Trace.events == 0 && result == sentinel);
            else {
                require(calls[0] == 1);
                if (mode >= 4 && mode <= 6) require(caught == expected && Trace.events == 1 && result == sentinel);
                else if (mode == 7) require(caught != null && caught.getClass() == ArrayStoreException.class && Trace.events == 1 && result == sentinel && values[0].equals("owned initial"));
                else require(caught == null && Trace.events == 12 && result == desired);
                require(Trace.result == desired && Trace.values == (mode == 12 ? alternate : values));
                require(Trace.context == (mode == 12 ? sentinel : context) && Trace.selector == (mode == 12 ? 0 : selector));
                if (mode < 4 || mode > 7) if (values != null && values.length != 0) require(values[0] == payload);
            }
        }
        System.out.println("attempts=2,array-nominal-varargs=matched");
    }
}
