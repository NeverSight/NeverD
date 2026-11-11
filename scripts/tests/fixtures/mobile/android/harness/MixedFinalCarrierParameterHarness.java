import fixture.*;
import java.lang.reflect.*;

public final class MixedFinalCarrierParameterHarness {
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
        if (Trace.events != expected) throw new AssertionError("effect order: " + Trace.events);
        Trace.events = 0;
    }
    private static void failure(Runnable call, Class<? extends Throwable> kind,
                                RuntimeException same, int effects) {
        try { call.run(); throw new AssertionError("failure missing"); }
        catch (Throwable error) {
            if (error.getClass() != kind || (same != null && error != same))
                throw new AssertionError("failure type or identity changed", error);
            if (same != null) identities++;
            failures++;
        }
        events(effects);
    }
    @SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
    public static void main(String[] args) throws Exception {
        Receiver<Object> receiver = new Receiver<>();
        Function raw = receiver;
        Context context = new Context();
        Callback<Context, Result> callback = new Callback<Context, Result>() {};
        long[] scalars = {0, -1, 1, Long.MIN_VALUE, Long.MAX_VALUE,
                          Integer.MIN_VALUE, Integer.MAX_VALUE, 0x0123456789abcdefL};
        Boolean[] results = {Boolean.TRUE, Boolean.FALSE, null,
                             new Boolean(true), new Boolean(false)};
        for (Context first : new Context[] {context, null})
            for (Callback third : new Callback[] {callback, null})
                for (Boolean result : results)
                    for (long scalar : scalars) {
                        receiver.result = result;
                        identity(raw.invoke(first, new CarrierB(scalar), third), result);
                        identity(receiver.context, first); identity(receiver.callback, third);
                        value(Trace.second, scalar); events(23);
                        identity(receiver.apply(first, ~scalar, third), result);
                        identity(receiver.context, first); identity(receiver.callback, third);
                        value(Trace.second, ~scalar); events(3);
                    }
        CarrierB carrier = new CarrierB(19);
        failure(() -> raw.invoke("bad", carrier, callback), ClassCastException.class, null, 0);
        failure(() -> raw.invoke(context, "bad", callback), ClassCastException.class, null, 0);
        failure(() -> raw.invoke(context, null, callback), NullPointerException.class, null, 0);
        failure(() -> raw.invoke(context, carrier, "bad"), ClassCastException.class, null, 2);
        failure(() -> raw.invoke("bad", "bad", "bad"), ClassCastException.class, null, 0);
        RuntimeException problem = new IllegalStateException("owned mixed callback failure");
        Trace.secondFailure = problem;
        failure(() -> raw.invoke(context, carrier, "bad"), IllegalStateException.class, problem, 2);
        Trace.secondFailure = null;
        Trace.targetFailure = problem;
        failure(() -> raw.invoke(context, carrier, callback), IllegalStateException.class, problem, 23);
        failure(() -> receiver.apply(context, 3, callback), IllegalStateException.class, problem, 3);
        Trace.targetFailure = null;
        Receiver<Object> absent = null;
        failure(() -> absent.invoke(context, carrier, callback), NullPointerException.class, null, 0);
        boolean rebuilt = args[0].equals("rebuilt");
        Method bridge = Receiver.class.getMethod("invoke", Object.class, Object.class, Object.class);
        if (bridge.isBridge() == rebuilt || bridge.isSynthetic() == rebuilt)
            throw new AssertionError("bridge flags differ from report");
        Type edge = Receiver.class.getGenericInterfaces()[0];
        if (rebuilt ? edge != Function.class : !(edge instanceof ParameterizedType))
            throw new AssertionError("interface reflection differs from report");
        if (!rebuilt) {
            Type nested = ((ParameterizedType) edge).getActualTypeArguments()[2];
            Type bound = ((ParameterizedType) nested).getActualTypeArguments()[1];
            if (!(bound instanceof WildcardType) ||
                !java.util.Arrays.equals(((WildcardType) bound).getUpperBounds(), new Type[] {Result.class}))
                throw new AssertionError("original final bound changed");
        }
        Method target = Receiver.class.getMethod("apply", Context.class, long.class, Callback.class);
        Type[] parameter = ((ParameterizedType) target.getGenericParameterTypes()[2]).getActualTypeArguments();
        if (!(parameter[0] instanceof WildcardType) || parameter[1] != Result.class ||
            !java.util.Arrays.equals(((WildcardType) parameter[0]).getLowerBounds(), new Type[] {Context.class}) ||
            target.getReturnType() != Boolean.class || target.isBridge() || target.isSynthetic())
            throw new AssertionError("original complete callback contract changed");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(rebuilt ? "reflection=reported-raw-interface" : "reflection=original");
    }
}
