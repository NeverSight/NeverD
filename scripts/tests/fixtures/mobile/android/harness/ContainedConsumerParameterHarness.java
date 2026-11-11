import fixture.*;
import java.lang.reflect.*;

public final class ContainedConsumerParameterHarness {
    private static int values, identities, failures, calls, callbackCalls;
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("reference or exception identity changed");
        identities++;
    }
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("value or call order changed");
        values++;
    }
    private static final class Recorder implements Consumer<Object> {
        int calls;
        Object last;
        RuntimeException failure;
        public void resume(Object input) {
            Trace.events = Trace.events * 10 + 2;
            calls++; last = input;
            if (failure != null) throw failure;
        }
    }
    private static void call(Receiver receiver, Recorder consumer, Object actual,
                             Value result, Value input, int bits) {
        same(actual, result); same(receiver.last, input);
        same(receiver.seen, consumer); same(consumer.last, input);
        value(Trace.events, 123); value(receiver.calls, ++calls);
        value(consumer.calls, ++callbackCalls); Trace.events = 0;
        if (result != null) value(((Value) actual).bits, bits);
    }
    private static void fail(Runnable action, Class<?> kind, RuntimeException sentinel, int events) {
        Trace.events = 0;
        try { action.run(); throw new AssertionError("missing original exception"); }
        catch (RuntimeException error) {
            if (error.getClass() != kind || Trace.events != events)
                throw new AssertionError("exception type or callback order changed", error);
            if (sentinel != null) same(error, sentinel);
            failures++;
        }
        Trace.events = 0;
    }
    public static void main(String[] args) throws Exception {
        Receiver receiver = new Receiver(); Service api = receiver;
        Recorder callback = new Recorder();
        for (int number : new int[]{0, 1, -1, Integer.MIN_VALUE, Integer.MAX_VALUE, 0x12345678, -0x12345678})
            for (Value input : new Value[]{new Value(number), null})
                for (Value result : new Value[]{new Value(number), null}) {
                    receiver.result = result;
                    call(receiver, callback, receiver.request(input, callback), result, input, number);
                    call(receiver, callback, api.request(input, callback), result, input, number);
                    if (result != null) {
                        result.bits = number ^ 31;
                        call(receiver, callback, api.request(input, callback), result, input, number ^ 31);
                    }
                }
        RuntimeException sentinel = new IllegalStateException("owned consumer failure");
        Trace.targetFailure = sentinel;
        fail(() -> receiver.request(null, callback), IllegalStateException.class, sentinel, 1);
        fail(() -> api.request(null, callback), IllegalStateException.class, sentinel, 1);
        Trace.targetFailure = null; callback.failure = sentinel;
        fail(() -> receiver.request(null, callback), IllegalStateException.class, sentinel, 12);
        fail(() -> api.request(null, callback), IllegalStateException.class, sentinel, 12);
        callback.failure = null;
        fail(() -> receiver.request(null, null), NullPointerException.class, null, 1);
        fail(() -> api.request(null, null), NullPointerException.class, null, 1);
        Receiver absent = null; Service absentApi = null;
        fail(() -> absent.request(null, null), NullPointerException.class, null, 0);
        fail(() -> absentApi.request(null, null), NullPointerException.class, null, 0);
        boolean original = args[0].equals("original");
        boolean unbounded = args.length > 1 && args[1].equals("unbounded");
        Method target = Receiver.class.getMethod("request", Value.class, Consumer.class);
        Type parameter = ((ParameterizedType) target.getGenericParameterTypes()[1]).getActualTypeArguments()[0];
        if (unbounded && original) {
            if (!(parameter instanceof WildcardType) || ((WildcardType) parameter).getLowerBounds().length != 0 ||
                    !java.util.Arrays.equals(((WildcardType) parameter).getUpperBounds(), new Type[]{Object.class}))
                throw new AssertionError("original unbounded consumer changed");
        } else if (original || unbounded) {
            if (!(parameter instanceof WildcardType) ||
                    !java.util.Arrays.equals(((WildcardType) parameter).getLowerBounds(), new Type[]{Value.class}))
                throw new AssertionError("consumer lower bound differs from original or reported parameter");
        } else if (parameter != Object.class) throw new AssertionError("reported inherited parameter missing");
        Type field = ((ParameterizedType) Receiver.class.getField("seen").getGenericType()).getActualTypeArguments()[0];
        if (!java.util.Arrays.equals(((WildcardType) field).getLowerBounds(), unbounded ? new Type[]{} : new Type[]{Value.class}) ||
                target.getGenericReturnType() != Object.class || target.isBridge() || target.isSynthetic())
            throw new AssertionError("original field, result or method flags changed");
        Method contract = Service.class.getMethod("request", Value.class, Consumer.class);
        Type required = ((ParameterizedType) contract.getGenericParameterTypes()[1]).getActualTypeArguments()[0];
        if (unbounded ? !(required instanceof WildcardType) ||
                !java.util.Arrays.equals(((WildcardType) required).getLowerBounds(), new Type[]{Value.class}) : required != Object.class)
            throw new AssertionError("complete input interface contract changed");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(original ? "reflection=original" : "reflection=reported-consumer-parameter");
    }
}
