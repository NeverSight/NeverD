import fixture.*;
import java.lang.reflect.*;

public final class InterfaceMethodFormalHarness {
    private static final class Provided implements Bound {}
    private static final class OtherProvided implements OtherBound {}
    private static int identities, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("reference identity changed");
        identities++;
    }
    private static void events(int expected) {
        if (Trace.events != expected) throw new AssertionError("effects changed: " + Trace.events);
        Trace.events = 0;
    }
    private static void failure(Runnable call, RuntimeException expected) {
        try { call.run(); throw new AssertionError("failure missing"); }
        catch (RuntimeException actual) {
            if (expected == null) {
                if (actual.getClass() != NullPointerException.class)
                    throw new AssertionError("null receiver changed", actual);
            } else {
                identity(actual, expected);
                if (actual.getClass() != IllegalStateException.class)
                    throw new AssertionError("exception type changed", actual);
            }
            failures++;
        }
        events(expected == null ? 0 : 1);
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static final class Overriding extends View<Object> {
        public <X extends Bound> Pair<X, X> make(Pair<Object, X> input) {
            Trace.events = Trace.events * 10 + 5;
            last = input; return stored;
        }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        View<Object> view = new View<>(); Contract api = view;
        Pair[] inputs = {new Pair<>(new Object(), new Provided()),
                         new Pair<>("text", new OtherProvided()), new Pair<>(null, new Object()), null};
        Pair[] outputs = {new Pair<>(new Provided(), new Provided()), new Pair<>("raw", new Object()), null};
        for (Pair stored : outputs) for (Pair input : inputs) {
            view.stored = stored;
            identity(view.make(input), stored); identity(view.last, input); events(1);
            identity(api.make(input), stored); identity(view.last, input); events(1);
        }
        Overriding override = new Overriding(); override.stored = outputs[0];
        View<Object> viewOverride = override; Contract apiOverride = override;
        identity(viewOverride.make(inputs[0]), outputs[0]); events(5);
        identity(apiOverride.make(inputs[0]), outputs[0]); events(5);
        RuntimeException problem = new IllegalStateException("owned bound failure");
        Trace.targetFailure = problem;
        failure(() -> view.make(null), problem); failure(() -> api.make(null), problem);
        identity(apiOverride.make(null), outputs[0]); events(5);
        Trace.targetFailure = null;
        View<Object> absent = null; Contract absentApi = null;
        failure(() -> absent.make(null), null); failure(() -> absentApi.make(null), null);
        Method make = View.class.getMethod("make", Pair.class);
        TypeVariable<Method> variable = make.getTypeParameters()[0];
        if (!variable.getName().equals("R") || !variable.getBounds()[0].equals(Bound.class))
            throw new AssertionError("method bound or name changed");
        Type[] input = ((ParameterizedType) make.getGenericParameterTypes()[0]).getActualTypeArguments();
        Type[] output = ((ParameterizedType) make.getGenericReturnType()).getActualTypeArguments();
        if (!input[0].equals(View.class.getTypeParameters()[0]) || !input[1].equals(variable)
                || !output[0].equals(variable) || !output[1].equals(variable))
            throw new AssertionError("class and method scope changed");
        for (Method method : View.class.getDeclaredMethods())
            if (method.isBridge()) throw new AssertionError("unnecessary bridge introduced");
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridges=0");
        System.out.println("nominal-bounds-and-full-scopes=retained,raw-identity-and-virtual-dispatch=matched");
    }
}
