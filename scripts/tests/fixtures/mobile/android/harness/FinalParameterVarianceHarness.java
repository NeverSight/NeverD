import fixture.*;
import java.lang.reflect.*;

public final class FinalParameterVarianceHarness {
    private static int identities, failures;
    private static Input expectedInput;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("variance identity changed");
        identities++;
    }
    private static void events(int expected) {
        if (Trace.events != expected) throw new AssertionError("variance effects changed");
        Trace.events = 0;
    }
    private static void failure(Runnable call, Class<? extends Throwable> kind,
                                RuntimeException same, int effects) {
        try { call.run(); throw new AssertionError("failure missing"); }
        catch (Throwable e) {
            if (e.getClass() != kind || (same != null && e != same))
                throw new AssertionError("variance failure changed", e);
            if (same != null) identities++;
            failures++;
        }
        events(effects);
    }
    private static final class Callback implements Mapper<Input, Value> {
        public Value result;
        public Value map(Input value) {
            Trace.events = Trace.events * 10 + 2;
            identity(value, expectedInput);
            if (Trace.firstFailure != null) throw Trace.firstFailure;
            return result;
        }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Receiver<Object> receiver = new Receiver<>(); Function api = receiver;
        Callback mapper = new Callback();
        for (Input input : new Input[]{new Input(), null})
            for (Value output : new Value[]{new Value(), null})
                for (Box box : new Box[]{new Box<Boolean>(Boolean.TRUE), null}) {
                    receiver.input = expectedInput = input; mapper.result = output;
                    identity(receiver.invoke(mapper, box), output);
                    identity(receiver.last, mapper); identity(receiver.box, box); events(12);
                    identity(receiver.invoke((Object) mapper, (Object) box), output);
                    identity(receiver.last, mapper); identity(receiver.box, box); events(12);
                    identity(api.invoke(mapper, box), output);
                    identity(receiver.last, mapper); identity(receiver.box, box); events(12);
                }
        failure(() -> api.invoke(new Object(), new Object()), ClassCastException.class, null, 0);
        failure(() -> api.invoke(mapper, new Object()), ClassCastException.class, null, 0);
        failure(() -> api.invoke(null, null), NullPointerException.class, null, 1);
        RuntimeException problem = new IllegalStateException("owned final variance failure");
        Trace.targetFailure = problem;
        failure(() -> api.invoke(mapper, null), IllegalStateException.class, problem, 1);
        Trace.targetFailure = null; Trace.firstFailure = problem;
        failure(() -> receiver.invoke(mapper, null), IllegalStateException.class, problem, 12);
        Trace.firstFailure = null;
        Function absent = null;
        failure(() -> absent.invoke(null, null), NullPointerException.class, null, 0);
        boolean original = args.length != 0 && args[0].equals("original");
        Method target = Receiver.class.getMethod("invoke", Mapper.class, Box.class);
        Type result = ((ParameterizedType) target.getGenericParameterTypes()[0]).getActualTypeArguments()[1];
        if (original ? result != Value.class : !(result instanceof WildcardType)
                || ((WildcardType) result).getUpperBounds()[0] != Value.class)
            throw new AssertionError("target variance differs from report");
        Method bridge = Receiver.class.getMethod("invoke", Object.class, Object.class);
        if (!bridge.isBridge() || !bridge.isSynthetic()) throw new AssertionError("bridge flags changed");
        Type parentInput = ((ParameterizedType) Receiver.class.getGenericInterfaces()[0]).getActualTypeArguments()[0];
        Type parentResult = ((ParameterizedType) parentInput).getActualTypeArguments()[1];
        if (!(parentResult instanceof WildcardType)
                || ((WildcardType) parentResult).getUpperBounds()[0] != Value.class)
            throw new AssertionError("complete parent variance changed");
        System.out.println("identities=" + identities + ",failures=" + failures);
        System.out.println(original ? "reflection=original" : "reflection=reported-final-parameter");
    }
}
