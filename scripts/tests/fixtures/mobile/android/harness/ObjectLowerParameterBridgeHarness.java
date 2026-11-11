import fixture.*;
import java.lang.reflect.*;

public final class ObjectLowerParameterBridgeHarness {
    private static int values, identities, failures, calls, callbackCalls;
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("reference identity changed");
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
    private static final class StringConsumer implements Consumer<String> {
        public void resume(String input) { throw new AssertionError("wrong generic bridge cast passed"); }
    }
    private static void call(Receiver<?> receiver, Recorder consumer, Object actual,
                             Object result, Object payload) {
        same(actual, result); same(receiver.seen, consumer); same(consumer.last, payload);
        value(Trace.events, 123); value(receiver.calls, ++calls);
        value(consumer.calls, ++callbackCalls); Trace.events = 0;
    }
    private static void fail(Runnable action, Class<?> kind, RuntimeException sentinel, int events) {
        Trace.events = 0;
        try { action.run(); throw new AssertionError("missing original exception"); }
        catch (RuntimeException error) {
            if (error.getClass() != kind || Trace.events != events)
                throw new AssertionError("exception type or effect order changed", error);
            if (sentinel != null) same(error, sentinel);
            failures++;
        }
        Trace.events = 0;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Receiver<Object> receiver = new Receiver<>(); Function raw = receiver;
        Recorder callback = new Recorder();
        for (int number : new int[]{0, 1, -1, Integer.MIN_VALUE, Integer.MAX_VALUE, 0x12345678, -0x12345678})
            for (Object payload : new Object[]{new Object(), "text", null, Integer.valueOf(number), new Value(number), new int[]{number}})
                for (Object result : new Object[]{new Object(), null}) {
                    receiver.payload = payload; receiver.result = result;
                    call(receiver, callback, receiver.invoke(callback), result, payload);
                    call(receiver, callback, raw.invoke(callback), result, payload);
                }
        RuntimeException sentinel = new IllegalStateException("owned Object lower failure");
        Trace.targetFailure = sentinel;
        fail(() -> receiver.invoke(callback), IllegalStateException.class, sentinel, 1);
        fail(() -> raw.invoke(callback), IllegalStateException.class, sentinel, 1);
        Trace.targetFailure = null; callback.failure = sentinel;
        fail(() -> receiver.invoke(callback), IllegalStateException.class, sentinel, 12);
        fail(() -> raw.invoke(callback), IllegalStateException.class, sentinel, 12);
        callback.failure = null;
        fail(() -> raw.invoke(new Object()), ClassCastException.class, null, 0);
        fail(() -> receiver.invoke((Consumer<Object>) null), NullPointerException.class, null, 1);
        fail(() -> raw.invoke(null), NullPointerException.class, null, 1);
        Receiver<Object> absent = null; Function absentApi = null;
        fail(() -> absent.invoke(callback), NullPointerException.class, null, 0);
        fail(() -> absentApi.invoke(callback), NullPointerException.class, null, 0);
        receiver.payload = new Object();
        fail(() -> raw.invoke(new StringConsumer()), ClassCastException.class, null, 1);
        boolean original = args[0].equals("original");
        Method target = Receiver.class.getMethod("invoke", Consumer.class);
        Type parameter = ((ParameterizedType) target.getGenericParameterTypes()[0]).getActualTypeArguments()[0];
        if (original ? parameter != Object.class : !(parameter instanceof WildcardType) ||
                !java.util.Arrays.equals(((WildcardType) parameter).getLowerBounds(), new Type[]{Object.class}))
            throw new AssertionError("target parameter differs from original or reported projection");
        if (((ParameterizedType) Receiver.class.getField("seen").getGenericType()).getActualTypeArguments()[0] != Object.class ||
                target.getGenericReturnType() != Object.class || target.isBridge() || target.isSynthetic())
            throw new AssertionError("original field, result or target flags changed");
        Method bridge = Receiver.class.getMethod("invoke", Object.class);
        if (!bridge.isBridge() || !bridge.isSynthetic()) throw new AssertionError("original erased bridge flags changed");
        Type contract = ((ParameterizedType) Receiver.class.getGenericInterfaces()[0]).getActualTypeArguments()[0];
        Type argument = ((ParameterizedType) contract).getActualTypeArguments()[0];
        if (!java.util.Arrays.equals(((WildcardType) argument).getLowerBounds(), new Type[]{Object.class}))
            throw new AssertionError("original complete inherited Object lower contract changed");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(original ? "reflection=original" : "reflection=reported-Object-lower-parameter");
    }
}
