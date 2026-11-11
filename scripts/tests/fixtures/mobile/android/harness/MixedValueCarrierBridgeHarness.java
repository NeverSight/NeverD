import fixture.*;
import java.lang.reflect.*;

public final class MixedValueCarrierBridgeHarness {
    private static int values, identities, failures;
    private static void value(long actual, long expected) {
        if (actual != expected) throw new AssertionError("wide value changed");
        values++;
    }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("reference changed");
        identities++;
    }
    private static void events(int expected) {
        if (Trace.events != expected) throw new AssertionError("effect order changed: " + Trace.events);
        Trace.events = 0;
    }
    private static void result(Receiver<?> r, Object actual, Object first, Object second) {
        identity(actual, first); identity(r.first, first); identity(r.second, second);
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
    @SuppressWarnings({"unchecked", "rawtypes"})
    public static void main(String[] args) throws Exception {
        Receiver<Object> receiver = new Receiver<>();
        Function<CarrierA,CarrierB,CarrierB,CarrierA> typed = (Function) receiver;
        Function raw = receiver;
        long[] cases = {0L, -1L, 1L, Long.MIN_VALUE, Long.MAX_VALUE,
                        Integer.MIN_VALUE, Integer.MAX_VALUE, 0x0123456789abcdefL};
        CarrierA first = new CarrierA(17);
        CarrierB second = new CarrierB(29);
        for (long third : cases) {
            result(receiver, typed.invoke(first, second, new CarrierB(third)), first, second);
            value(Trace.second, third); events(23);
            result(receiver, receiver.apply(first, second, ~third), first, second);
            value(Trace.second, ~third); events(3);
        }
        result(receiver, typed.invoke(null, second, second), null, second); events(23);
        result(receiver, typed.invoke(first, null, second), first, null); events(23);
        failure(() -> raw.invoke("bad", second, second), ClassCastException.class, null, 0);
        failure(() -> raw.invoke(first, "bad", second), ClassCastException.class, null, 0);
        failure(() -> raw.invoke(first, second, "bad"), ClassCastException.class, null, 0);
        failure(() -> raw.invoke(first, second, null), NullPointerException.class, null, 0);
        RuntimeException problem = new IllegalStateException("owned mixed callback failure");
        Trace.secondFailure = problem;
        failure(() -> typed.invoke(first, second, second), IllegalStateException.class, problem, 2);
        Trace.secondFailure = null; Trace.targetFailure = problem;
        failure(() -> typed.invoke(first, second, second), IllegalStateException.class, problem, 23);
        failure(() -> receiver.apply(first, second, 31), IllegalStateException.class, problem, 3);
        Trace.targetFailure = null;
        Trace.firstFailure = problem; Trace.boxFailure = problem;
        result(receiver, typed.invoke(first, second, second), first, second); events(23);
        Trace.firstFailure = Trace.boxFailure = null;
        Receiver<Object> absent = null;
        failure(() -> absent.invoke(first, second, second), NullPointerException.class, null, 0);
        failure(() -> absent.apply(first, second, 1), NullPointerException.class, null, 0);
        boolean recovered = args[0].equals("recovered");
        Method bridge = Receiver.class.getMethod("invoke", Object.class, Object.class, Object.class);
        if (bridge.isBridge() == recovered || bridge.isSynthetic() == recovered)
            throw new AssertionError("bridge flags differ from report");
        Type contract = Receiver.class.getGenericInterfaces()[0];
        if (recovered ? !contract.equals(Function.class) : !(contract instanceof ParameterizedType))
            throw new AssertionError("interface reflection differs from report");
        Method target = Receiver.class.getMethod("apply", CarrierA.class, CarrierB.class, long.class);
        if (target.getReturnType() != CarrierA.class || target.isBridge() || target.isSynthetic())
            throw new AssertionError("mixed target ABI changed");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(recovered ? "reflection=reported-raw-interface" : "reflection=original");
    }
}
