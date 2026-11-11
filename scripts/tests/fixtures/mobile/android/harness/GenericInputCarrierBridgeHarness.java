import fixture.*;
import java.lang.reflect.*;

public final class GenericInputCarrierBridgeHarness {
    private static int values, identities, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("generic reference identity changed");
        identities++;
    }
    private static void value(int expected) {
        if (Trace.first != expected) throw new AssertionError("primitive input changed");
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
        public int intValue() {
            Trace.events = Trace.events * 10 + 2;
            if (Trace.firstFailure != null) throw Trace.firstFailure;
            return -73;
        }
        public long longValue() { return -73; }
        public float floatValue() { return -73; }
        public double doubleValue() { return -73; }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Receiver<Object, Object> receiver = new Receiver<>(); Function api = receiver;
        Box<Object>[] lefts = new Box[]{new Box<>(new Object()), new Box<>("text"), null};
        CarrierA[] rights = {new CarrierA(5), new CarrierA(-7), null};
        Number[] numbers = {Integer.valueOf(0), Integer.valueOf(42), Integer.valueOf(Integer.MIN_VALUE),
                            Integer.valueOf(Integer.MAX_VALUE), Double.valueOf(5.5), new CountingNumber()};
        int[] expected = {0, 42, Integer.MIN_VALUE, Integer.MAX_VALUE, 5, -73};
        for (Box<Object> left : lefts) for (CarrierA right : rights)
            for (int index = 0; index < numbers.length; index++) {
                identity(receiver.apply(left, right, expected[index]), left);
                identity(receiver.first, left); identity(receiver.second, right); value(expected[index]); events(1);
                identity(receiver.invoke(left, right, numbers[index]), left);
                identity(receiver.first, left); identity(receiver.second, right); value(expected[index]);
                events(numbers[index] instanceof CountingNumber ? 21 : 1);
                identity(api.invoke(left, right, numbers[index]), left);
                identity(receiver.first, left); identity(receiver.second, right); value(expected[index]);
                events(numbers[index] instanceof CountingNumber ? 21 : 1);
            }
        Object wrong = new Object();
        failure(() -> receiver.invoke(wrong, null, null), ClassCastException.class, null, 0);
        failure(() -> api.invoke(wrong, null, null), ClassCastException.class, null, 0);
        failure(() -> receiver.invoke(null, wrong, null), ClassCastException.class, null, 0);
        failure(() -> api.invoke(null, wrong, null), ClassCastException.class, null, 0);
        failure(() -> receiver.invoke(null, null, wrong), ClassCastException.class, null, 0);
        failure(() -> api.invoke(null, null, wrong), ClassCastException.class, null, 0);
        failure(() -> receiver.invoke(null, null, null), NullPointerException.class, null, 0);
        failure(() -> api.invoke(null, null, null), NullPointerException.class, null, 0);
        RuntimeException problem = new IllegalStateException("owned generic input failure");
        Trace.targetFailure = problem;
        failure(() -> receiver.apply(null, null, 0), IllegalStateException.class, problem, 1);
        failure(() -> receiver.invoke(null, null, Integer.valueOf(0)), IllegalStateException.class, problem, 1);
        failure(() -> api.invoke(null, null, Integer.valueOf(0)), IllegalStateException.class, problem, 1);
        Trace.targetFailure = null; Trace.firstFailure = problem;
        Number effectful = new CountingNumber();
        failure(() -> receiver.invoke(null, null, effectful), IllegalStateException.class, problem, 2);
        failure(() -> api.invoke(null, null, effectful), IllegalStateException.class, problem, 2);
        Trace.firstFailure = null;
        Receiver<Object, Object> absent = null; Function absentApi = null;
        failure(() -> absent.apply(null, null, 0), NullPointerException.class, null, 0);
        failure(() -> absent.invoke(null, null, null), NullPointerException.class, null, 0);
        failure(() -> absentApi.invoke(null, null, null), NullPointerException.class, null, 0);
        Method apply = Receiver.class.getMethod("apply", Box.class, CarrierA.class, int.class);
        Type original = Receiver.class.getTypeParameters()[0];
        if (!((ParameterizedType) apply.getGenericParameterTypes()[0]).getActualTypeArguments()[0].equals(original)
                || !((ParameterizedType) apply.getGenericReturnType()).getActualTypeArguments()[0].equals(original))
            throw new AssertionError("complete target scope changed");
        boolean input = args.length != 0 && args[0].equals("original");
        Method invoke = Receiver.class.getMethod("invoke", Object.class, Object.class, Object.class);
        if (invoke.isBridge() != input || invoke.isSynthetic() != input)
            throw new AssertionError("bridge flags differ from report");
        Type contract = Receiver.class.getGenericInterfaces()[0];
        if (input) {
            Type[] parameters = ((ParameterizedType) contract).getActualTypeArguments();
            if (!parameters[0].equals(apply.getGenericParameterTypes()[0]) || parameters[1] != CarrierA.class
                    || parameters[2] != Integer.class || !parameters[3].equals(apply.getGenericReturnType()))
                throw new AssertionError("original implemented contract changed");
        } else if (contract != Function.class) throw new AssertionError("reported raw interface missing");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(input ? "reflection=original" : "reflection=reported-raw-interface");
    }
}
