import fixture.*;
import java.lang.reflect.*;

public final class ReferenceResultBoxingHarness {
    private static int values, identities, failures;
    private static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    private static void same(Object actual, Object expected) { check(actual == expected); identities++; }
    private static void events(int expected) { check(Trace.events == expected); Trace.events = 0; }
    private static void failure(Runnable action, RuntimeException expected) {
        try { action.run(); throw new AssertionError(); }
        catch (RuntimeException actual) {
            if (expected != null) same(actual, expected);
            else check(actual.getClass() == NullPointerException.class);
            failures++;
        }
    }
    public static void main(String[] args) throws Exception {
        Receiver<Object> receiver = new Receiver<>();
        Function function = receiver;
        RuntimeException sentinel = new IllegalArgumentException("owned");
        Trace.targetFailure = sentinel;
        failure(() -> function.invoke(), sentinel); events(1);
        check(Trace.initializations == 0);
        Trace.targetFailure = null;
        Object a = new Object(), b = new Object();
        Payload<Object,Object>[] payloads = new Payload[] {new Payload<>(a,b),new Payload<>(null,null),null};
        for (int repeat = 0; repeat < 3; repeat++) {
            for (Payload<Object,Object> payload : payloads) {
                receiver.selected = payload;
                Carrier first = (Carrier) function.invoke();
                events(repeat == 0 && payload == payloads[0] ? 12 : 1);
                same(first.payload(), payload);
                Carrier second = (Carrier) function.invoke(); events(1);
                same(second.payload(), payload);
                check(first != second); values++;
                same(receiver.storage(), payload); events(1);
                if (payload != null) {
                    payload.left = repeat % 2 == 0 ? b : null;
                    payload.right = a;
                    same(first.payload().left, payload.left);
                    same(second.payload().right, a);
                    values++;
                }
            }
        }
        check(Trace.initializations == 1);
        Trace.targetFailure = sentinel;
        failure(() -> function.invoke(), sentinel); events(1);
        failure(() -> receiver.storage(), sentinel); events(1);
        Trace.targetFailure = null;
        failure(() -> ((Function) null).invoke(), null); events(0);
        Method bridge = Receiver.class.getDeclaredMethod("invoke");
        boolean original = args[0].equals("original");
        check(bridge.isBridge() == original && bridge.isSynthetic() == original);
        Type contract = Receiver.class.getGenericInterfaces()[0];
        if (original) {
            ParameterizedType callback = (ParameterizedType) contract;
            ParameterizedType carrier = (ParameterizedType) callback.getActualTypeArguments()[0];
            check(carrier.getRawType() == Carrier.class);
            check(carrier.getActualTypeArguments()[0] == Object.class);
            check(carrier.getActualTypeArguments()[1] == Element.class);
        } else check(contract == Function.class);
        ParameterizedType representation = (ParameterizedType) Carrier.class.getDeclaredField("backing").getGenericType();
        same(representation.getActualTypeArguments()[0], Object.class);
        same(representation.getActualTypeArguments()[1], Object.class);
        check(Receiver.class.getDeclaredMethod("storage").getGenericReturnType().equals(representation));
        check(Carrier.class.getDeclaredMethod("payload").getGenericReturnType().equals(representation));
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("fixed-representation=retained,fresh-boxing=matched,initialization-after-success=matched");
    }
}
