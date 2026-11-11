import fixture.*;
import java.lang.reflect.*;

public final class ResultOnlyCarrierBridgeHarness {
    private static int values, identities, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("argument identity changed");
        identities++;
    }
    private static void value(long actual, long expected) {
        if (actual != expected) throw new AssertionError("wide value changed");
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
        Receiver<Object, Object> receiver = new Receiver<>();
        Function api = receiver;
        long[] numbers = {0, 1, -1, Long.MIN_VALUE, Long.MAX_VALUE,
                          0x0123456789abcdefL, -0x0123456789abcdefL};
        Object[] inputs = {new Object(), "text", null, Integer.valueOf(7),
                           new int[]{2, 3}, new CarrierB(11)};
        for (long number : numbers) for (Object input : inputs) {
            receiver.value = number;
            value(receiver.apply(input), number); identity(receiver.last, input); events(1);
            CarrierB direct = (CarrierB) receiver.invoke(input);
            value(direct.value, number); identity(receiver.last, input); events(14);
            CarrierB indirect = (CarrierB) api.invoke(input);
            value(indirect.value, number); identity(receiver.last, input); events(14);
            if (direct == indirect) throw new AssertionError("original allocation changed");
            direct.value = number ^ 31;
            if (indirect.value != number) throw new AssertionError("carrier instances aliased");
        }
        RuntimeException problem = new IllegalStateException("owned result-carrier failure");
        Trace.targetFailure = problem;
        failure(() -> receiver.apply(null), IllegalStateException.class, problem, 1);
        failure(() -> receiver.invoke(null), IllegalStateException.class, problem, 1);
        failure(() -> api.invoke(null), IllegalStateException.class, problem, 1);
        Trace.targetFailure = null; Trace.boxFailure = problem;
        failure(() -> receiver.invoke(null), IllegalStateException.class, problem, 14);
        failure(() -> api.invoke(null), IllegalStateException.class, problem, 14);
        Trace.boxFailure = null;
        Receiver<Object, Object> absent = null; Function absentApi = null;
        failure(() -> absent.apply(null), NullPointerException.class, null, 0);
        failure(() -> absent.invoke(null), NullPointerException.class, null, 0);
        failure(() -> absentApi.invoke(null), NullPointerException.class, null, 0);
        Method apply = Receiver.class.getMethod("apply", Object.class);
        if (apply.getReturnType() != long.class ||
                !apply.getGenericParameterTypes()[0].equals(Receiver.class.getTypeParameters()[0]))
            throw new AssertionError("original target scope changed");
        Method invoke = Receiver.class.getMethod("invoke", Object.class);
        boolean original = args.length != 0 && args[0].equals("original");
        if (invoke.isBridge() != original || invoke.isSynthetic() != original)
            throw new AssertionError("bridge metadata difference was not as reported");
        Type contract = Receiver.class.getGenericInterfaces()[0];
        if (original) {
            ParameterizedType parameterized = (ParameterizedType) contract;
            Type[] parameters = parameterized.getActualTypeArguments();
            if (!parameters[0].equals(Receiver.class.getTypeParameters()[0]) || parameters[1] != CarrierB.class)
                throw new AssertionError("original full interface contract changed");
        } else if (contract != Function.class) throw new AssertionError("reported raw interface missing");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(original ? "reflection=original" : "reflection=reported-raw-interface");
    }
}
