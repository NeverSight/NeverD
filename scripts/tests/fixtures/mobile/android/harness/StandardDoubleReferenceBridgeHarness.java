import fixture.*;
import java.lang.reflect.*;
import java.math.BigInteger;

public final class StandardDoubleReferenceBridgeHarness {
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
        public double doubleValue() {
            Trace.events = Trace.events * 10 + 2;
            if (Trace.firstFailure != null) throw Trace.firstFailure;
            return Double.longBitsToDouble(0x7ff8000000000043L);
        }
        public int intValue() { throw new AssertionError("wrong int conversion"); }
        public long longValue() { throw new AssertionError("wrong long conversion"); }
        public float floatValue() { throw new AssertionError("wrong float conversion"); }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Receiver<Object, Object> receiver = new Receiver<>(); Function api = receiver;
        Number[] numbers = {Double.valueOf(0.0), Double.valueOf(-0.0), Double.valueOf(1.0),
                            Double.valueOf(-1.0), Double.valueOf(Double.POSITIVE_INFINITY),
                            Double.valueOf(Double.NEGATIVE_INFINITY),
                            Double.valueOf(Double.longBitsToDouble(0x7ff8000000000042L)),
                            Float.valueOf(-1.25f), new CountingNumber()};
        long[] expected = {0, 0x8000000000000000L, 0x3ff0000000000000L,
                           0xbff0000000000000L, 0x7ff0000000000000L, 0xfff0000000000000L,
                           0x7ff8000000000042L, 0xbff4000000000000L, 0x7ff8000000000043L};
        Object[] outputs = {new Object(), "text", null, new int[]{1, 2}};
        for (Object output : outputs) for (int index = 0; index < numbers.length; index++) {
            receiver.result = output;
            identity(receiver.apply(Double.longBitsToDouble(expected[index])), output); value(expected[index]); events(1);
            identity(receiver.invoke(numbers[index]), output); value(expected[index]);
            events(numbers[index] instanceof CountingNumber ? 21 : 1);
            identity(api.invoke(numbers[index]), output); value(expected[index]);
            events(numbers[index] instanceof CountingNumber ? 21 : 1);
        }
        failure(() -> receiver.invoke(new Object()), ClassCastException.class, null, 0);
        failure(() -> api.invoke(new Object()), ClassCastException.class, null, 0);
        failure(() -> receiver.invoke(null), NullPointerException.class, null, 0);
        failure(() -> api.invoke(null), NullPointerException.class, null, 0);
        RuntimeException problem = new IllegalStateException("owned double conversion failure");
        Trace.firstFailure = problem;
        failure(() -> receiver.invoke(new CountingNumber()), IllegalStateException.class, problem, 2);
        failure(() -> api.invoke(new CountingNumber()), IllegalStateException.class, problem, 2);
        Trace.firstFailure = null; Trace.targetFailure = problem;
        failure(() -> receiver.apply(7), IllegalStateException.class, problem, 1);
        failure(() -> receiver.invoke(Double.valueOf(7)), IllegalStateException.class, problem, 1);
        failure(() -> api.invoke(Double.valueOf(7)), IllegalStateException.class, problem, 1);
        Trace.targetFailure = null;
        Receiver<Object, Object> absent = null; Function absentApi = null;
        failure(() -> absent.apply(7), NullPointerException.class, null, 0);
        failure(() -> absent.invoke(null), NullPointerException.class, null, 0);
        failure(() -> absentApi.invoke(null), NullPointerException.class, null, 0);
        Method apply = Receiver.class.getMethod("apply", double.class);
        if (!apply.getGenericReturnType().equals(Receiver.class.getTypeParameters()[0]))
            throw new AssertionError("original target return scope changed");
        boolean original = args.length != 0 && args[0].equals("original");
        Method invoke = Receiver.class.getMethod("invoke", Object.class);
        if (invoke.isBridge() != original || invoke.isSynthetic() != original)
            throw new AssertionError("bridge metadata differs from report");
        Type contract = Receiver.class.getGenericInterfaces()[0];
        if (original) {
            Type[] parameters = ((ParameterizedType) contract).getActualTypeArguments();
            if (parameters[0] != Double.class || !parameters[1].equals(Receiver.class.getTypeParameters()[0]))
                throw new AssertionError("original full interface contract changed");
        } else if (contract != Function.class) throw new AssertionError("reported raw interface missing");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(original ? "reflection=original" : "reflection=reported-raw-interface");
    }
}
