import fixture.*;
import java.lang.reflect.*;
import java.math.BigInteger;

public final class SharedLongReferenceBridgeHarness {
    private static int values, identities, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("captured identity changed");
        identities++;
    }
    private static Object typed(Method apply, Object receiver, long input) {
        try { return apply.invoke(receiver, input); }
        catch (InvocationTargetException e) { throw (RuntimeException) e.getCause(); }
        catch (ReflectiveOperationException e) { throw new AssertionError(e); }
    }
    private static void effects(int expected, long value) {
        if (Trace.events != expected || Trace.first != value)
            throw new AssertionError("wide value or effect order changed");
        values++;
        Trace.events = 0;
    }
    private static void failure(Runnable call, Class<? extends Throwable> kind,
                                RuntimeException same, int effects) {
        try { call.run(); throw new AssertionError("failure missing"); }
        catch (Throwable e) {
            if (e.getClass() != kind || (same != null && e != same))
                throw new AssertionError("exception type or identity changed", e);
            if (same != null) identities++;
            failures++;
        }
        if (Trace.events != effects) throw new AssertionError("failure effects changed");
        Trace.events = 0;
    }
    private static final class CountingNumber extends Number {
        public long longValue() {
            Trace.events = Trace.events * 10 + 2;
            if (Trace.firstFailure != null) throw Trace.firstFailure;
            return 73;
        }
        public int intValue() { throw new AssertionError("wrong narrow conversion"); }
        public float floatValue() { return 73; }
        public double doubleValue() { return 73; }
    }
    public static void main(String[] args) throws Exception {
        Object first = Owner.make(new Object()), second = Owner.another(new Object());
        if (first == second || first.getClass() != second.getClass())
            throw new AssertionError("shared class or separate instances changed");
        Class<?> worker = first.getClass();
        Method apply = worker.getMethod("apply", long.class);
        Field result = worker.getField("result");
        Number[] numbers = {Long.valueOf(0), Long.valueOf(-1), Long.valueOf(Long.MIN_VALUE),
                            Long.valueOf(Long.MAX_VALUE), Double.valueOf(-91.75),
                            new CountingNumber(), new BigInteger("18446744073709551619")};
        long[] expected = {0, -1, Long.MIN_VALUE, Long.MAX_VALUE, -91, 73, 3};
        for (Object receiver : new Object[]{first, second}) {
            Function api = (Function) receiver;
            for (Object output : new Object[]{new Object(), "text", null}) {
                result.set(receiver, output);
                for (int index = 0; index < numbers.length; index++) {
                    identity(typed(apply, receiver, expected[index]), output);
                    effects(1, expected[index]);
                    identity(api.invoke(numbers[index]), output);
                    effects(numbers[index] instanceof CountingNumber ? 21 : 1, expected[index]);
                }
            }
            failure(() -> api.invoke(new Object()), ClassCastException.class, null, 0);
            failure(() -> api.invoke(null), NullPointerException.class, null, 0);
            RuntimeException problem = new IllegalStateException("owned shared long failure");
            Trace.firstFailure = problem;
            failure(() -> api.invoke(new CountingNumber()), IllegalStateException.class, problem, 2);
            Trace.firstFailure = null; Trace.targetFailure = problem;
            failure(() -> typed(apply, receiver, 7), IllegalStateException.class, problem, 1);
            failure(() -> api.invoke(Long.valueOf(7)), IllegalStateException.class, problem, 1);
            Trace.targetFailure = null;
        }
        boolean original = args.length != 0 && args[0].equals("original");
        Method invoke = worker.getMethod("invoke", Object.class);
        if (invoke.isBridge() != original || invoke.isSynthetic() != original)
            throw new AssertionError("bridge flags differ from report");
        Type contract = worker.getGenericInterfaces()[0];
        if (original) {
            Method enclosing = Owner.class.getMethod("make", Object.class);
            Type s = enclosing.getTypeParameters()[0];
            if (!worker.isAnonymousClass() || !enclosing.equals(worker.getEnclosingMethod())
                    || !s.equals(apply.getGenericReturnType()) || !s.equals(result.getGenericType()))
                throw new AssertionError("original enclosing formal changed");
            Type[] parameters = ((ParameterizedType) contract).getActualTypeArguments();
            if (parameters[0] != Long.class || !s.equals(parameters[1]))
                throw new AssertionError("original scoped interface contract changed");
        } else if (contract != Function.class || apply.getGenericReturnType() != Object.class
                || result.getGenericType() != Object.class || worker.isAnonymousClass())
            throw new AssertionError("reported source metadata changes missing");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(original ? "reflection=original" : "reflection=reported-shared-scope");
    }
}
