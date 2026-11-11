import fixture.*;
import java.lang.reflect.*;
import java.math.BigInteger;

public final class StandardLongReferenceBridgeHarness {
    private static int values, identities, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("scoped result identity changed");
        identities++;
    }
    private static void value(long expected) {
        if (Trace.first != expected) throw new AssertionError("wide parameter changed");
        values++;
    }
    private static void events(int expected) {
        if (Trace.events != expected) throw new AssertionError("effect order changed: " + Trace.events);
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
        events(effects);
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
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Receiver<Object, Object> receiver = new Receiver<>(); Function api = receiver;
        Number[] numbers = {Long.valueOf(0), Long.valueOf(1), Long.valueOf(-1),
                            Long.valueOf(Long.MIN_VALUE), Long.valueOf(Long.MAX_VALUE),
                            Double.valueOf(-91.75), Float.valueOf(12.5f), new CountingNumber(),
                            new BigInteger("18446744073709551619")};
        long[] expected = {0, 1, -1, Long.MIN_VALUE, Long.MAX_VALUE, -91, 12, 73, 3};
        Object[] outputs = {new Object(), "text", null, new int[]{1, 2}};
        for (Object output : outputs) for (int index = 0; index < numbers.length; index++) {
            receiver.result = output;
            identity(receiver.apply(expected[index]), output); value(expected[index]); events(1);
            identity(receiver.invoke(numbers[index]), output); value(expected[index]);
            events(numbers[index] instanceof CountingNumber ? 21 : 1);
            identity(api.invoke(numbers[index]), output); value(expected[index]);
            events(numbers[index] instanceof CountingNumber ? 21 : 1);
        }
        failure(() -> receiver.invoke(new Object()), ClassCastException.class, null, 0);
        failure(() -> api.invoke(new Object()), ClassCastException.class, null, 0);
        failure(() -> receiver.invoke(null), NullPointerException.class, null, 0);
        failure(() -> api.invoke(null), NullPointerException.class, null, 0);
        RuntimeException problem = new IllegalStateException("owned long conversion failure");
        Trace.firstFailure = problem;
        failure(() -> receiver.invoke(new CountingNumber()), IllegalStateException.class, problem, 2);
        failure(() -> api.invoke(new CountingNumber()), IllegalStateException.class, problem, 2);
        Trace.firstFailure = null; Trace.targetFailure = problem;
        failure(() -> receiver.apply(7), IllegalStateException.class, problem, 1);
        failure(() -> receiver.invoke(Long.valueOf(7)), IllegalStateException.class, problem, 1);
        failure(() -> api.invoke(Long.valueOf(7)), IllegalStateException.class, problem, 1);
        Trace.targetFailure = null;
        Receiver<Object, Object> absent = null; Function absentApi = null;
        failure(() -> absent.apply(7), NullPointerException.class, null, 0);
        failure(() -> absent.invoke(null), NullPointerException.class, null, 0);
        failure(() -> absentApi.invoke(null), NullPointerException.class, null, 0);
        Method apply = Receiver.class.getMethod("apply", long.class);
        if (!apply.getGenericReturnType().equals(Receiver.class.getTypeParameters()[0]))
            throw new AssertionError("original target return scope changed");
        boolean original = args.length != 0 && args[0].equals("original");
        Method invoke = Receiver.class.getMethod("invoke", Object.class);
        if (invoke.isBridge() != original || invoke.isSynthetic() != original)
            throw new AssertionError("bridge metadata differs from report");
        Type contract = Receiver.class.getGenericInterfaces()[0];
        if (original) {
            Type[] parameters = ((ParameterizedType) contract).getActualTypeArguments();
            if (parameters[0] != Long.class || !parameters[1].equals(Receiver.class.getTypeParameters()[0]))
                throw new AssertionError("original full interface contract changed");
        } else if (contract != Function.class) throw new AssertionError("reported raw interface missing");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(original ? "reflection=original" : "reflection=reported-raw-interface");
    }
}
