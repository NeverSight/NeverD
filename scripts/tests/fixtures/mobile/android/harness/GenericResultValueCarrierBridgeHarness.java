import fixture.*;
import java.lang.reflect.*;

public final class GenericResultValueCarrierBridgeHarness {
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
        Function<CarrierA,CarrierB,Box<CarrierA>> typed = (Function) receiver;
        Function raw = receiver;
        long[] cases = {0L, -1L, 1L, Long.MIN_VALUE, Long.MAX_VALUE,
                        Integer.MIN_VALUE, Integer.MAX_VALUE, 0x0123456789abcdefL};
        for (int i = 0; i < cases.length; i++) {
            CarrierA first = new CarrierA(cases[i]);
            CarrierB second = new CarrierB(cases[cases.length - i - 1]);
            Box<CarrierA> box = new Box<>(first);
            receiver.result = box;
            Box<CarrierA> actual = typed.invoke(first, second);
            identity(actual, box); identity(actual.value, first);
            value(Trace.first, first.value); value(Trace.second, second.value); events(123);
            identity(receiver.apply(second.value, first.value), box);
            value(Trace.first, second.value); value(Trace.second, first.value); events(3);
        }
        CarrierA first = new CarrierA(17);
        CarrierB second = new CarrierB(29);
        receiver.result = null;
        identity(typed.invoke(first, second), null); events(123);
        failure(() -> raw.invoke("bad", second), ClassCastException.class, null, 0);
        failure(() -> raw.invoke(null, second), NullPointerException.class, null, 0);
        failure(() -> raw.invoke(first, "bad"), ClassCastException.class, null, 1);
        failure(() -> raw.invoke(first, null), NullPointerException.class, null, 1);
        RuntimeException problem = new IllegalStateException("owned generic callback failure");
        Trace.firstFailure = problem;
        failure(() -> typed.invoke(first, second), IllegalStateException.class, problem, 1);
        Trace.firstFailure = null; Trace.secondFailure = problem;
        failure(() -> typed.invoke(first, second), IllegalStateException.class, problem, 12);
        Trace.secondFailure = null; Trace.targetFailure = problem;
        failure(() -> typed.invoke(first, second), IllegalStateException.class, problem, 123);
        failure(() -> receiver.apply(31, 47), IllegalStateException.class, problem, 3);
        Trace.targetFailure = null;
        Receiver<Object> absent = null;
        failure(() -> absent.invoke(first, second), NullPointerException.class, null, 0);
        failure(() -> absent.apply(1, 2), NullPointerException.class, null, 0);
        boolean recovered = args[0].equals("recovered");
        Method bridge = Receiver.class.getMethod("invoke", Object.class, Object.class);
        if (bridge.isBridge() == recovered || bridge.isSynthetic() == recovered)
            throw new AssertionError("bridge flags differ from report");
        Type contract = Receiver.class.getGenericInterfaces()[0];
        if (recovered ? !contract.equals(Function.class) : !(contract instanceof ParameterizedType))
            throw new AssertionError("interface reflection differs from report");
        Method target = Receiver.class.getMethod("apply", long.class, long.class);
        if (!target.getGenericReturnType().getTypeName().equals("fixture.Box<fixture.CarrierA>")
                || target.isBridge() || target.isSynthetic())
            throw new AssertionError("complete generic target result changed");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(recovered ? "reflection=reported-raw-interface" : "reflection=original");
    }
}
