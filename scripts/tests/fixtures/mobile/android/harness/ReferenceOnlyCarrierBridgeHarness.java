import fixture.*;
import java.lang.reflect.*;

public final class ReferenceOnlyCarrierBridgeHarness {
    private static int values, identities, failures, calls;
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original reference changed");
        identities++;
    }
    private static void value(long actual, long expected) {
        if (actual != expected) throw new AssertionError("value or effect changed");
        values++;
    }
    private static void call(Receiver<Object, Object> receiver, Object actual,
                             Carrier<Object> expected, Object input, long bits, Object payload) {
        same(actual, expected); same(receiver.last, input);
        value(Trace.events, 1); value(receiver.calls, ++calls);
        Trace.events = 0;
        if (expected != null) {
            value(((Carrier<?>) actual).bits, bits);
            same(((Carrier<?>) actual).value, payload);
        }
    }
    private static void fail(Runnable action, Class<?> kind, RuntimeException sentinel, int events) {
        Trace.events = 0;
        try { action.run(); throw new AssertionError("missing original exception"); }
        catch (RuntimeException error) {
            if (error.getClass() != kind || Trace.events != events)
                throw new AssertionError("exception type or call order changed", error);
            if (sentinel != null) same(error, sentinel);
            failures++;
        }
        Trace.events = 0;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Receiver<Object, Object> receiver = new Receiver<>();
        Function raw = receiver;
        Object payload = new Object();
        Object[] inputs = {new Object(), "text", null, Integer.valueOf(7), new int[]{2, 3}, payload};
        for (long number : new long[]{0, 1, -1, Long.MIN_VALUE, Long.MAX_VALUE,
                                      0x0123456789abcdefL, -0x0123456789abcdefL})
            for (Object input : inputs)
                for (Carrier<Object> result : new Carrier[]{new Carrier<>(payload, number),
                                                           new Carrier<>(payload, number), null}) {
                    receiver.value = result;
                    call(receiver, receiver.apply(input), result, input, number, payload);
                    call(receiver, receiver.invoke(input), result, input, number, payload);
                    call(receiver, raw.invoke(input), result, input, number, payload);
                    if (result != null) {
                        result.bits = number ^ 31; result.value = input;
                        call(receiver, raw.invoke(input), result, input, number ^ 31, input);
                    }
                }
        RuntimeException sentinel = new IllegalStateException("owned reference failure");
        Trace.targetFailure = sentinel;
        fail(() -> receiver.apply(null), IllegalStateException.class, sentinel, 1);
        fail(() -> receiver.invoke(null), IllegalStateException.class, sentinel, 1);
        fail(() -> raw.invoke(null), IllegalStateException.class, sentinel, 1);
        Trace.targetFailure = null;
        Receiver<Object, Object> absent = null; Function absentApi = null;
        fail(() -> absent.apply(null), NullPointerException.class, null, 0);
        fail(() -> absent.invoke(null), NullPointerException.class, null, 0);
        fail(() -> absentApi.invoke(null), NullPointerException.class, null, 0);
        TypeVariable<?> formal = Receiver.class.getTypeParameters()[0];
        Method typed = Receiver.class.getMethod("apply", Object.class);
        ParameterizedType result = (ParameterizedType) typed.getGenericReturnType();
        if (!typed.getGenericParameterTypes()[0].equals(formal) || result.getRawType() != Carrier.class ||
                !result.getActualTypeArguments()[0].equals(formal) ||
                !Receiver.class.getField("value").getGenericType().equals(result))
            throw new AssertionError("complete original target or field scope changed");
        boolean original = args[0].equals("original");
        Method bridge = Receiver.class.getMethod("invoke", Object.class);
        if (bridge.isBridge() != original || bridge.isSynthetic() != original)
            throw new AssertionError("bridge flags differ from reported projection");
        Type contract = Receiver.class.getGenericInterfaces()[0];
        if (original) {
            Type[] arguments = ((ParameterizedType) contract).getActualTypeArguments();
            if (!arguments[0].equals(formal) || !arguments[1].equals(result))
                throw new AssertionError("original specialized interface changed");
        } else if (contract != Function.class) throw new AssertionError("reported raw source edge missing");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(original ? "reflection=original" : "reflection=reported-raw-interface");
    }
}
