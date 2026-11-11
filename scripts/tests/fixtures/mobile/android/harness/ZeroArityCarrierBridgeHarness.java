import fixture.*;
import java.lang.reflect.*;

public final class ZeroArityCarrierBridgeHarness {
    private static int values, identities, failures;
    private static void value(long actual, long expected) {
        if (actual != expected) throw new AssertionError("wide result changed");
        values++;
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
                throw new AssertionError("exception type or identity changed", e);
            if (same != null) identities++;
            failures++;
        }
        events(effects);
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Receiver<Object, Object> receiver = new Receiver<>(); Function api = receiver;
        long[] numbers = {0, 1, -1, Long.MIN_VALUE, Long.MAX_VALUE,
                          0x0123456789abcdefL, -0x0123456789abcdefL};
        for (long number : numbers) {
            receiver.value = number;
            value(receiver.apply(), number); events(1);
            CarrierB direct = (CarrierB) receiver.invoke(); value(direct.value, number); events(14);
            CarrierB indirect = (CarrierB) api.invoke(); value(indirect.value, number); events(14);
            if (direct == indirect) throw new AssertionError("original allocation changed");
            direct.value = number ^ 31;
            if (indirect.value != number) throw new AssertionError("independent instances aliased");
        }
        RuntimeException problem = new IllegalStateException("owned zero-argument failure");
        Trace.targetFailure = problem;
        failure(() -> receiver.apply(), IllegalStateException.class, problem, 1);
        failure(() -> receiver.invoke(), IllegalStateException.class, problem, 1);
        failure(() -> api.invoke(), IllegalStateException.class, problem, 1);
        Trace.targetFailure = null; Trace.boxFailure = problem;
        failure(() -> receiver.invoke(), IllegalStateException.class, problem, 14);
        failure(() -> api.invoke(), IllegalStateException.class, problem, 14);
        Trace.boxFailure = null;
        Receiver<Object, Object> absent = null; Function absentApi = null;
        failure(() -> absent.apply(), NullPointerException.class, null, 0);
        failure(() -> absent.invoke(), NullPointerException.class, null, 0);
        failure(() -> absentApi.invoke(), NullPointerException.class, null, 0);
        Method apply = Receiver.class.getMethod("apply");
        if (apply.getReturnType() != long.class || apply.getParameterCount() != 0)
            throw new AssertionError("original target ABI changed");
        boolean original = args.length != 0 && args[0].equals("original");
        Method invoke = Receiver.class.getMethod("invoke");
        if (invoke.isBridge() != original || invoke.isSynthetic() != original)
            throw new AssertionError("bridge difference was not reported");
        Type contract = Receiver.class.getGenericInterfaces()[0];
        if (original) {
            if (((ParameterizedType) contract).getActualTypeArguments()[0] != CarrierB.class)
                throw new AssertionError("original result contract changed");
        } else if (contract != Function.class) throw new AssertionError("reported raw interface missing");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(original ? "reflection=original" : "reflection=reported-raw-interface");
    }
}
