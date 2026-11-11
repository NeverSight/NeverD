import fixture.*;
import java.lang.reflect.*;

public final class PrimitiveArrayResultBridgeHarness {
    private static int values, identities, failures;
    private static void value(long actual, long expected) {
        if (actual != expected) throw new AssertionError("integer value changed");
        values++;
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("array or exception identity changed");
        identities++;
    }
    private static final class ObservedNumber extends Number {
        private final int id, number;
        ObservedNumber(int id, int number) { this.id = id; this.number = number; }
        public int intValue() {
            Trace.events = Trace.events * 10 + id;
            RuntimeException error = id == 1 ? Trace.firstFailure : Trace.secondFailure;
            if (error != null) throw error;
            return number;
        }
        public long longValue() { return number; }
        public float floatValue() { return number; }
        public double doubleValue() { return number; }
    }
    private static void fails(Runnable operation, Class<?> type, RuntimeException sentinel, int events) {
        Trace.events = 0;
        try { operation.run(); throw new AssertionError("missing exception"); }
        catch (RuntimeException error) {
            if (!type.isInstance(error)) throw new AssertionError("exception type changed", error);
            if (sentinel != null) same(error, sentinel);
            value(Trace.events, events);
            failures++;
        }
    }
    public static void main(String[] args) throws Exception {
        Receiver<Object> receiver = new Receiver<>();
        Function raw = receiver;
        int calls = 0;
        for (int first : new int[] {Integer.MIN_VALUE, -1, 0, 1, Integer.MAX_VALUE})
            for (int second : new int[] {Integer.MIN_VALUE, -1, 0, 1, Integer.MAX_VALUE})
                for (int[] result : new int[][] {new int[] {17, 29}, null}) {
                    receiver.result = result;
                    Trace.events = 0;
                    same(raw.invoke(new ObservedNumber(1, first), new ObservedNumber(2, second)), result);
                    value(Trace.events, 123); value(Trace.first, first); value(Trace.second, second);
                    value(receiver.calls, ++calls);
                    Trace.events = 0;
                    same(receiver.apply(first, second), result);
                    value(Trace.events, 3); value(Trace.first, first); value(Trace.second, second);
                    value(receiver.calls, ++calls);
                    if (result != null) {
                        result[0] = first;
                        same(receiver.result, result);
                        value(receiver.result[0], first);
                    }
                }
        receiver.result = new int[] {3, 5};
        for (int first : new int[] {Integer.MIN_VALUE, -1, 0, 1, Integer.MAX_VALUE}) {
            Trace.events = 0;
            same(raw.invoke(Integer.valueOf(first), Integer.valueOf(~first)), receiver.result);
            value(Trace.events, 3); value(Trace.first, first); value(Trace.second, ~first);
            value(receiver.calls, ++calls);
        }
        ObservedNumber left = new ObservedNumber(1, 11), right = new ObservedNumber(2, 22);
        fails(() -> raw.invoke(new Object(), right), ClassCastException.class, null, 0);
        fails(() -> raw.invoke(null, right), NullPointerException.class, null, 0);
        fails(() -> raw.invoke(left, new Object()), ClassCastException.class, null, 1);
        fails(() -> raw.invoke(left, null), NullPointerException.class, null, 1);
        RuntimeException sentinel = new IllegalStateException("owned conversion failure");
        Trace.firstFailure = sentinel;
        fails(() -> raw.invoke(left, right), IllegalStateException.class, sentinel, 1);
        Trace.firstFailure = null;
        Trace.secondFailure = sentinel;
        fails(() -> raw.invoke(left, right), IllegalStateException.class, sentinel, 12);
        Trace.secondFailure = null;
        Trace.targetFailure = sentinel;
        fails(() -> raw.invoke(left, right), IllegalStateException.class, sentinel, 123);
        fails(() -> receiver.apply(11, 22), IllegalStateException.class, sentinel, 3);
        Trace.targetFailure = null;
        boolean recovered = args[0].equals("recovered");
        Method bridge = Receiver.class.getMethod("invoke", Object.class, Object.class);
        if (bridge.isBridge() == recovered || bridge.isSynthetic() == recovered)
            throw new AssertionError("bridge flags differ from receipt");
        if (Receiver.class.getMethod("apply", int.class, int.class).getGenericReturnType() != int[].class
                || Receiver.class.getField("result").getGenericType() != int[].class)
            throw new AssertionError("array result or field reflection changed");
        Type api = Receiver.class.getGenericInterfaces()[0];
        if (recovered) {
            if (api != Function.class) throw new AssertionError("raw interface projection missing");
        } else {
            Type[] arguments = ((ParameterizedType) api).getActualTypeArguments();
            if (arguments.length != 3 || arguments[0] != Integer.class || arguments[1] != Integer.class
                    || arguments[2] != int[].class) throw new AssertionError("original array contract lost");
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(recovered ? "reflection=reported-raw-interface" : "reflection=original");
    }
}
