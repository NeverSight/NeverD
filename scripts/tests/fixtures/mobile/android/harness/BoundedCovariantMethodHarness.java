import fixture.*;
import java.lang.reflect.*;

public final class BoundedCovariantMethodHarness {
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
        public <X extends Bound> ResultLeaf<X> make(Pair<Object, X> input) {
            Trace.events = Trace.events * 10 + 5;
            last = input; return stored;
        }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        View<Object> view = new View<>(); Contract api = view; RootContract root = view;
        Pair[] inputs = {new Pair<>(new Object(), new Bound()),
                         new Pair<>("text", new OtherBound()), new Pair<>(null, new Object()), null};
        ResultLeaf[] outputs = {new ResultLeaf<>(new Bound()), new ResultLeaf<>(new OtherBound()), new ResultLeaf<>(null), null};
        for (ResultLeaf stored : outputs) for (Pair input : inputs) {
            view.stored = stored;
            identity(view.make(input), stored); identity(view.last, input); events(1);
            identity(api.make(input), stored); identity(view.last, input); events(1);
            identity(root.make(input), stored); identity(view.last, input); events(1);
        }
        Overriding override = new Overriding(); override.stored = outputs[0];
        View<Object> viewOverride = override; Contract apiOverride = override; RootContract rootOverride = override;
        identity(viewOverride.make(inputs[0]), outputs[0]); events(5);
        identity(apiOverride.make(inputs[0]), outputs[0]); events(5);
        identity(rootOverride.make(inputs[0]), outputs[0]); events(5);
        RuntimeException problem = new IllegalStateException("owned bound failure");
        Trace.targetFailure = problem;
        failure(() -> view.make(null), problem); failure(() -> api.make(null), problem);
        failure(() -> root.make(null), problem);
        identity(apiOverride.make(null), outputs[0]); events(5);
        Trace.targetFailure = null;
        View<Object> absent = null; Contract absentApi = null; RootContract absentRoot = null;
        failure(() -> absent.make(null), null); failure(() -> absentApi.make(null), null);
        failure(() -> absentRoot.make(null), null);
        Method make = View.class.getMethod("make", Pair.class);
        TypeVariable<Method> variable = make.getTypeParameters()[0];
        if (!variable.getName().equals("R") || !variable.getBounds()[0].equals(Bound.class))
            throw new AssertionError("method bound or name changed");
        Type[] input = ((ParameterizedType) make.getGenericParameterTypes()[0]).getActualTypeArguments();
        Type[] output = ((ParameterizedType) make.getGenericReturnType()).getActualTypeArguments();
        if (!input[0].equals(View.class.getTypeParameters()[0]) || !input[1].equals(variable)
                || output.length != 1 || !output[0].equals(variable))
            throw new AssertionError("class and method scope changed");
        int bridges = 0;
        for (Method method : View.class.getDeclaredMethods())
            if (method.isBridge()) {
                if (!method.isSynthetic()) throw new AssertionError("bridge flags changed");
                bridges++;
            }
        if (bridges != 2) throw new AssertionError("original covariant bridge inventory changed");
        int interfaceBridges = 0;
        for (Method method : Contract.class.getDeclaredMethods())
            if (method.isBridge()) {
                if (!method.isSynthetic() || !method.isDefault())
                    throw new AssertionError("interface bridge flags changed");
                interfaceBridges++;
            }
        boolean omitted = args.length != 0 && args[0].equals("omitted");
        if (interfaceBridges != (omitted ? 0 : 1))
            throw new AssertionError("interface declaration difference was not as reported");
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridges=2");
        System.out.println("bounded-covariant-scopes=retained,original-bridge-dispatch-and-identity=matched");
        System.out.println("interface-bridge-count=" + interfaceBridges);
    }
}
