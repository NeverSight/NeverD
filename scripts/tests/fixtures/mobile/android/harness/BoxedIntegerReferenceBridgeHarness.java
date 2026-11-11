import fixture.*;
import java.lang.reflect.*;
import java.math.BigInteger;

public final class BoxedIntegerReferenceBridgeHarness {
    private static int values, identities, failures;
    private static void value(long actual, long expected) {
        if (actual != expected) throw new AssertionError("integer changed");
        values++;
    }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("identity changed");
        identities++;
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
                throw new AssertionError("failure type or identity changed", e);
            if (same != null) identities++;
            failures++;
        }
        events(effects);
    }
    private static final class CountingNumber extends Number {
        private final int value;
        CountingNumber(int value) { this.value = value; }
        public int intValue() {
            Trace.events = Trace.events * 10 + 1;
            if (Trace.firstFailure != null) throw Trace.firstFailure;
            return value;
        }
        public long longValue() { throw new AssertionError("wrong conversion"); }
        public float floatValue() { throw new AssertionError("wrong conversion"); }
        public double doubleValue() { throw new AssertionError("wrong conversion"); }
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    public static void main(String[] args) throws Exception {
        Receiver<Object> receiver = new Receiver<>();
        Function<Integer,Integer> typed = (Function) receiver;
        Function raw = receiver;
        int[] cases = {0, -1, 1, -128, 127, 128, Integer.MIN_VALUE, Integer.MAX_VALUE};
        for (int input : cases) {
            Integer actual = typed.invoke(Integer.valueOf(input));
            value(actual.intValue(), input); value(Trace.first, input); events(3);
            if (input >= -128 && input <= 127) identity(actual, Integer.valueOf(input));
            value(receiver.apply(~input).intValue(), ~input); value(Trace.first, ~input); events(3);
        }
        Number[] others = {Double.valueOf(17.75), Float.valueOf(-9.5f), new BigInteger("1099511627779")};
        for (Number input : others) {
            value(((Integer) raw.invoke(input)).intValue(), input.intValue());
            value(Trace.first, input.intValue()); events(3);
        }
        Number counted = new CountingNumber(31);
        value(((Integer) raw.invoke(counted)).intValue(), 31); value(Trace.first, 31); events(13);
        failure(() -> raw.invoke("bad"), ClassCastException.class, null, 0);
        failure(() -> raw.invoke(Boolean.TRUE), ClassCastException.class, null, 0);
        failure(() -> raw.invoke(null), NullPointerException.class, null, 0);
        RuntimeException problem = new IllegalStateException("owned scalar callback failure");
        Trace.firstFailure = problem;
        failure(() -> raw.invoke(counted), IllegalStateException.class, problem, 1);
        Trace.firstFailure = null; Trace.targetFailure = problem;
        failure(() -> raw.invoke(counted), IllegalStateException.class, problem, 13);
        failure(() -> receiver.apply(47), IllegalStateException.class, problem, 3);
        Trace.targetFailure = null;
        Receiver<Object> absent = null;
        failure(() -> absent.invoke(Integer.valueOf(1)), NullPointerException.class, null, 0);
        failure(() -> absent.apply(1), NullPointerException.class, null, 0);
        boolean recovered = args[0].equals("recovered");
        Method bridge = Receiver.class.getMethod("invoke", Object.class);
        if (bridge.isBridge() == recovered || bridge.isSynthetic() == recovered)
            throw new AssertionError("bridge flags differ from report");
        Type contract = Receiver.class.getGenericInterfaces()[0];
        if (recovered ? !contract.equals(Function.class) : !(contract instanceof ParameterizedType))
            throw new AssertionError("interface reflection differs from report");
        Method target = Receiver.class.getMethod("apply", int.class);
        if (target.getReturnType() != Integer.class || target.isBridge() || target.isSynthetic())
            throw new AssertionError("integer target ABI changed");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(recovered ? "reflection=reported-raw-interface" : "reflection=original");
    }
}
