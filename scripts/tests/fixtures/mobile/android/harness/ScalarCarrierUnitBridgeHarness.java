import fixture.*;
import java.lang.reflect.*;
import kotlin.Unit;

public final class ScalarCarrierUnitBridgeHarness {
    private static int values, identities, failures, calls;
    private static void value(long actual, long expected) {
        if (actual != expected) throw new AssertionError("integer value or initialization order changed");
        values++;
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("singleton or exception identity changed");
        identities++;
    }
    private static class ChildCarrier extends Carrier {
        ChildCarrier(int value) { super(value); }
        @Override public int unwrap() {
            Trace.events = Trace.events * 10 + 2;
            if (Trace.secondFailure != null) throw Trace.secondFailure;
            return value ^ 0x55555555;
        }
    }
    private static void result(Receiver receiver, Object returned, int number, int events) {
        same(returned, Unit.INSTANCE);
        value(Trace.events, events); value(Trace.first, number); value(receiver.seen, number);
        value(receiver.calls, ++calls);
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
        Carrier initial = new Carrier(Integer.MIN_VALUE);
        RuntimeException sentinel = new IllegalStateException("owned scalar failure");
        Trace.firstFailure = sentinel;
        fails(() -> raw.invoke(initial), IllegalStateException.class, sentinel, 1);
        Trace.firstFailure = null;
        Trace.targetFailure = sentinel;
        fails(() -> raw.invoke(initial), IllegalStateException.class, sentinel, 13);
        Trace.targetFailure = null;
        calls = 1;
        Trace.events = 0;
        result(receiver, raw.invoke(initial), Integer.MIN_VALUE, 132);
        for (int number : new int[] {Integer.MIN_VALUE, -65536, -1, 0, 1, 65536, Integer.MAX_VALUE}) {
            Carrier input = new Carrier(number);
            Trace.events = 0;
            result(receiver, raw.invoke(input), number, 13);
            Trace.events = 0;
            receiver.apply(number);
            value(Trace.events, 3); value(Trace.first, number); value(receiver.seen, number);
            value(receiver.calls, ++calls);
            input.value ^= 0x80000000;
            Trace.events = 0;
            result(receiver, raw.invoke(input), number ^ 0x80000000, 13);
        }
        for (int number : new int[] {Integer.MIN_VALUE, Integer.MAX_VALUE}) {
            Trace.events = 0;
            result(receiver, raw.invoke(new ChildCarrier(number)), number ^ 0x55555555, 23);
        }
        fails(() -> raw.invoke(new Object()), ClassCastException.class, null, 0);
        fails(() -> raw.invoke(null), NullPointerException.class, null, 0);
        Trace.firstFailure = sentinel;
        fails(() -> raw.invoke(initial), IllegalStateException.class, sentinel, 1);
        Trace.firstFailure = null;
        Trace.secondFailure = sentinel;
        fails(() -> raw.invoke(new ChildCarrier(1)), IllegalStateException.class, sentinel, 2);
        Trace.secondFailure = null;
        Trace.targetFailure = sentinel;
        fails(() -> raw.invoke(initial), IllegalStateException.class, sentinel, 13);
        fails(() -> receiver.apply(11), IllegalStateException.class, sentinel, 3);
        Trace.targetFailure = null;
        boolean recovered = args[0].equals("recovered");
        Method bridge = Receiver.class.getMethod("invoke", Object.class);
        if (bridge.isBridge() == recovered || bridge.isSynthetic() == recovered)
            throw new AssertionError("bridge flags differ from report");
        if (Carrier.class.getMethod("unwrap").getGenericReturnType() != int.class
                || Receiver.class.getMethod("apply", int.class).getGenericReturnType() != void.class)
            throw new AssertionError("scalar or void target reflection changed");
        Type api = Receiver.class.getGenericInterfaces()[0];
        if (recovered) {
            if (api != Function.class) throw new AssertionError("raw interface projection missing");
        } else {
            Type[] arguments = ((ParameterizedType) api).getActualTypeArguments();
            if (arguments.length != 2 || arguments[0] != Carrier.class || arguments[1] != Unit.class)
                throw new AssertionError("original scalar carrier contract lost");
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(recovered ? "reflection=reported-raw-interface" : "reflection=original");
    }
}
