import fixture.*;
import java.lang.reflect.*;

public final class StandardFloatReferenceBridgeHarness {
    private static int values, identities, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("input or result identity changed");
        identities++;
    }
    private static void value(float expected) {
        if (Trace.first != Float.floatToRawIntBits(expected))
            throw new AssertionError("float bits changed");
        values++;
    }
    private static void events(int expected) {
        if (Trace.events != expected) throw new AssertionError("effect order changed");
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
        public float floatValue() {
            Trace.events = Trace.events * 10 + 2;
            if (Trace.firstFailure != null) throw Trace.firstFailure;
            return 73.5f;
        }
        public int intValue() { throw new AssertionError("wrong integer conversion"); }
        public long longValue() { throw new AssertionError("wrong wide conversion"); }
        public double doubleValue() { return 73.5; }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Receiver receiver = new Receiver(); Function api = receiver;
        float nan = Float.intBitsToFloat(0x7fc01234);
        Number[] numbers = {Float.valueOf(0), Float.valueOf(-0.0f), Float.valueOf(Float.MIN_VALUE),
                            Float.valueOf(Float.MAX_VALUE), Float.valueOf(Float.POSITIVE_INFINITY),
                            Float.valueOf(Float.NEGATIVE_INFINITY), Float.valueOf(nan),
                            Double.valueOf(-91.75), Long.valueOf(Long.MAX_VALUE), new CountingNumber()};
        float[] expected = {0, -0.0f, Float.MIN_VALUE, Float.MAX_VALUE, Float.POSITIVE_INFINITY,
                            Float.NEGATIVE_INFINITY, nan, -91.75f, (float) Long.MAX_VALUE, 73.5f};
        Box[] boxes = {new Box<Boolean>(Boolean.TRUE), new Box<Object>("text"), null};
        for (Object output : new Object[]{new Object(), "result", null}) for (Box box : boxes)
            for (int index = 0; index < numbers.length; index++) {
                receiver.result = output;
                identity(receiver.apply(expected[index], box), output);
                identity(receiver.last, box); value(expected[index]); events(1);
                identity(receiver.invoke(numbers[index], box), output);
                identity(receiver.last, box); value(expected[index]);
                events(numbers[index] instanceof CountingNumber ? 21 : 1);
                identity(api.invoke(numbers[index], box), output);
                identity(receiver.last, box); value(expected[index]);
                events(numbers[index] instanceof CountingNumber ? 21 : 1);
            }
        failure(() -> receiver.invoke(new Object(), new Object()), ClassCastException.class, null, 0);
        failure(() -> api.invoke(null, new Object()), NullPointerException.class, null, 0);
        failure(() -> receiver.invoke(new CountingNumber(), new Object()), ClassCastException.class, null, 2);
        RuntimeException problem = new IllegalStateException("owned float conversion failure");
        Trace.firstFailure = problem;
        failure(() -> receiver.invoke(new CountingNumber(), new Object()), IllegalStateException.class, problem, 2);
        failure(() -> api.invoke(new CountingNumber(), null), IllegalStateException.class, problem, 2);
        Trace.firstFailure = null; Trace.targetFailure = problem;
        failure(() -> receiver.apply(7, null), IllegalStateException.class, problem, 1);
        failure(() -> receiver.invoke(Float.valueOf(7), null), IllegalStateException.class, problem, 1);
        failure(() -> api.invoke(Float.valueOf(7), null), IllegalStateException.class, problem, 1);
        Trace.targetFailure = null;
        Receiver absent = null; Function absentApi = null;
        failure(() -> absent.apply(7, null), NullPointerException.class, null, 0);
        failure(() -> absent.invoke(null, null), NullPointerException.class, null, 0);
        failure(() -> absentApi.invoke(null, null), NullPointerException.class, null, 0);
        Method apply = Receiver.class.getMethod("apply", float.class, Box.class);
        ParameterizedType input = (ParameterizedType) apply.getGenericParameterTypes()[1];
        WildcardType bound = (WildcardType) input.getActualTypeArguments()[0];
        if (bound.getLowerBounds().length != 1 || bound.getLowerBounds()[0] != Boolean.class)
            throw new AssertionError("complete original target input changed");
        boolean original = args.length != 0 && args[0].equals("original");
        Method invoke = Receiver.class.getMethod("invoke", Object.class, Object.class);
        if (invoke.isBridge() != original || invoke.isSynthetic() != original)
            throw new AssertionError("bridge flags differ from report");
        Type contract = Receiver.class.getGenericInterfaces()[0];
        if (original) {
            Type[] parameters = ((ParameterizedType) contract).getActualTypeArguments();
            if (parameters[0] != Float.class || !parameters[1].equals(input) || parameters[2] != Object.class)
                throw new AssertionError("original complete interface contract changed");
        } else if (contract != Function.class) throw new AssertionError("reported raw interface missing");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(original ? "reflection=original" : "reflection=reported-raw-interface");
    }
}
