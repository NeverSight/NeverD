import fixture.*;
import java.lang.reflect.*;

public final class ReferenceCarrierBridgeHarness {
    private static int values, identities, failures, calls;
    private static void value(long actual, long expected) {
        if (actual != expected) throw new AssertionError("value or event order changed");
        values++;
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("reference identity changed");
        identities++;
    }
    private static class ChildCarrier extends Carrier {
        ChildCarrier(Payload value) { super(value); }
        @Override public Payload unwrap() {
            Trace.events = Trace.events * 10 + 2;
            if (Trace.secondFailure != null) throw Trace.secondFailure;
            return value;
        }
    }
    private static void call(Receiver receiver, Object result, Payload payload, int events) {
        same(result, Boolean.valueOf(payload != null && (payload.value & 1) != 0));
        same(receiver.seen, payload);
        value(Trace.events, events); value(Trace.first, payload == null ? 0 : payload.value);
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
        for (int number : new int[] {Integer.MIN_VALUE, -1, 0, 1, Integer.MAX_VALUE}) {
            Payload<Object> owned = new Payload<>();
            owned.value = number;
            for (Payload payload : new Payload[] {owned, null}) {
                Trace.events = 0;
                call(receiver, raw.invoke(new Carrier(payload)), payload, 13);
                Trace.events = 0;
                call(receiver, receiver.apply(payload), payload, 3);
                if (payload != null) {
                    payload.value ^= 1;
                    Trace.events = 0;
                    call(receiver, raw.invoke(new Carrier(payload)), payload, 13);
                }
            }
        }
        Payload<Object> owned = new Payload<>();
        owned.value = 11;
        Trace.events = 0;
        call(receiver, raw.invoke(new ChildCarrier(owned)), owned, 23);
        Trace.events = 0;
        call(receiver, raw.invoke(new ChildCarrier(null)), null, 23);
        fails(() -> raw.invoke(new Object()), ClassCastException.class, null, 0);
        fails(() -> raw.invoke(null), NullPointerException.class, null, 0);
        RuntimeException sentinel = new IllegalStateException("owned reference failure");
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
        Method typed = Receiver.class.getMethod("apply", Payload.class);
        if (typed.getGenericParameterTypes()[0] != Payload.class || typed.getGenericReturnType() != Boolean.class
                || Receiver.class.getField("seen").getGenericType() != Payload.class)
            throw new AssertionError("original raw payload or target reflection changed");
        Type api = Receiver.class.getGenericInterfaces()[0];
        if (recovered) {
            if (api != Function.class) throw new AssertionError("raw interface projection missing");
        } else {
            Type[] arguments = ((ParameterizedType) api).getActualTypeArguments();
            if (arguments.length != 2 || arguments[0] != Carrier.class || arguments[1] != Boolean.class)
                throw new AssertionError("original reference carrier contract lost");
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(recovered ? "reflection=reported-raw-interface" : "reflection=original");
    }
}
