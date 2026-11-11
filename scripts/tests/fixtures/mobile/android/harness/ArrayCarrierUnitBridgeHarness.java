import fixture.*;
import java.lang.reflect.*;
import kotlin.Unit;

public final class ArrayCarrierUnitBridgeHarness {
    private static int values, identities, failures, calls;
    private static void value(long actual, long expected) {
        if (actual != expected) throw new AssertionError("value, bits or initialization order changed");
        values++;
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("array, singleton or exception identity changed");
        identities++;
    }
    private static class ChildCarrier extends Carrier {
        ChildCarrier(float[] value) { super(value); }
        @Override public float[] unwrap() {
            Trace.events = Trace.events * 10 + 2;
            if (Trace.secondFailure != null) throw Trace.secondFailure;
            return value;
        }
    }
    private static int bits(float[] array) {
        return array != null && array.length != 0 ? Float.floatToRawIntBits(array[0]) : 0;
    }
    private static void result(Receiver receiver, Object returned, float[] array, int before, int events) {
        same(returned, Unit.INSTANCE); same(receiver.seen, array);
        value(Trace.events, events); value(Trace.first, before); value(receiver.calls, ++calls);
        if (array != null && array.length != 0) value(bits(array), before ^ 0x80000000);
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
        float[] initial = {Float.intBitsToFloat(0x80000000), Float.intBitsToFloat(0x7fc00123)};
        RuntimeException sentinel = new IllegalStateException("owned array failure");
        Trace.firstFailure = sentinel;
        fails(() -> raw.invoke(new Carrier(initial)), IllegalStateException.class, sentinel, 1);
        Trace.firstFailure = null;
        Trace.targetFailure = sentinel;
        fails(() -> raw.invoke(new Carrier(initial)), IllegalStateException.class, sentinel, 13);
        Trace.targetFailure = null;
        calls = 1;
        Trace.events = 0;
        result(receiver, raw.invoke(new Carrier(initial)), initial, 0x80000000, 132);
        for (int word : new int[] {0, 0x80000000, 1, 0x7f7fffff, 0x7fc00123, 0x7f800000, 0xff800000})
            for (float[] array : new float[][] {new float[] {Float.intBitsToFloat(word)}, null, new float[0]}) {
                int before = bits(array);
                Trace.events = 0;
                result(receiver, raw.invoke(new Carrier(array)), array, before, 13);
                before = bits(array);
                Trace.events = 0;
                receiver.apply(array);
                same(receiver.seen, array);
                value(Trace.events, 3); value(Trace.first, before); value(receiver.calls, ++calls);
                if (array != null && array.length != 0) value(bits(array), before ^ 0x80000000);
            }
        float[] owned = {Float.intBitsToFloat(0x7fc00123)};
        Trace.events = 0;
        result(receiver, raw.invoke(new ChildCarrier(owned)), owned, 0x7fc00123, 23);
        Trace.events = 0;
        result(receiver, raw.invoke(new ChildCarrier(null)), null, 0, 23);
        fails(() -> raw.invoke(new Object()), ClassCastException.class, null, 0);
        fails(() -> raw.invoke(null), NullPointerException.class, null, 0);
        Trace.firstFailure = sentinel;
        fails(() -> raw.invoke(new Carrier(owned)), IllegalStateException.class, sentinel, 1);
        Trace.firstFailure = null;
        Trace.secondFailure = sentinel;
        fails(() -> raw.invoke(new ChildCarrier(owned)), IllegalStateException.class, sentinel, 2);
        Trace.secondFailure = null;
        Trace.targetFailure = sentinel;
        fails(() -> raw.invoke(new Carrier(owned)), IllegalStateException.class, sentinel, 13);
        fails(() -> receiver.apply(owned), IllegalStateException.class, sentinel, 3);
        Trace.targetFailure = null;
        boolean recovered = args[0].equals("recovered");
        Method bridge = Receiver.class.getMethod("invoke", Object.class);
        if (bridge.isBridge() == recovered || bridge.isSynthetic() == recovered)
            throw new AssertionError("bridge flags differ from report");
        if (Carrier.class.getMethod("unwrap").getGenericReturnType() != float[].class
                || Receiver.class.getMethod("apply", float[].class).getGenericReturnType() != void.class
                || Receiver.class.getField("seen").getGenericType() != float[].class)
            throw new AssertionError("array or void target reflection changed");
        Type api = Receiver.class.getGenericInterfaces()[0];
        if (recovered) {
            if (api != Function.class) throw new AssertionError("raw interface projection missing");
        } else {
            Type[] arguments = ((ParameterizedType) api).getActualTypeArguments();
            if (arguments.length != 2 || arguments[0] != Carrier.class || arguments[1] != Unit.class)
                throw new AssertionError("original array carrier contract lost");
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(recovered ? "reflection=reported-raw-interface" : "reflection=original");
    }
}
