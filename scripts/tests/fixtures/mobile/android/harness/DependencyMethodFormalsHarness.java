import fixture.*;
import java.lang.reflect.*;

public final class DependencyMethodFormalsHarness {
    private static int identities, states, failures;
    private interface Call {
        Mark run(Api receiver, RawBox<Payload> box) throws Throwable;
    }
    private static final class Overriding extends Leaf {
        public <N extends Mark> N select(RawBox<N> box) {
            Trace.events = Trace.events * 10 + 8;
            calls += 3; seen = box;
            if (failure != null) throw failure;
            return box == null ? null : box.value;
        }
    }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("method-owned SDK identity changed");
        identities++;
    }
    private static Object invoke(Method method, Object receiver, Object argument) throws Throwable {
        try { return method.invoke(receiver, argument); }
        catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    private static void formal(Method method, String name) {
        TypeVariable<Method>[] variables = method.getTypeParameters();
        if (variables.length != 1 || !variables[0].getName().equals(name)
                || !variables[0].getGenericDeclaration().equals(method)
                || variables[0].getBounds().length != 1
                || variables[0].getBounds()[0] != Mark.class
                || !method.getGenericReturnType().equals(variables[0])
                || method.getExceptionTypes().length != 0 || method.isBridge())
            throw new AssertionError("complete original method-owned SDK formal changed");
        ParameterizedType parameter = (ParameterizedType) method.getGenericParameterTypes()[0];
        if (parameter.getRawType() != RawBox.class
                || parameter.getActualTypeArguments().length != 1
                || !parameter.getActualTypeArguments()[0].equals(variables[0]))
            throw new AssertionError("invariant SDK parameter lost its original method scope");
    }
    private static int count(Api receiver) {
        return receiver instanceof Forwarder ? ((Forwarder) receiver).sink.calls : ((Parent) receiver).calls;
    }
    private static void state(Api receiver, RawBox<Payload> box, int before, int route) {
        RawBox<?> seen = receiver instanceof Forwarder ? ((Forwarder) receiver).sink.seen : ((Parent) receiver).seen;
        identity(seen, box);
        if (count(receiver) != before + (receiver instanceof Overriding ? 3 : 1))
            throw new AssertionError("generic provider state update changed");
        int event = receiver instanceof Forwarder ? 4 : receiver instanceof Overriding ? 8 : 7;
        if (Trace.events != (route >= 4 ? 60 + event : event))
            throw new AssertionError("SDK default and virtual provider effect order changed");
        Trace.events = 0;
        if (box != null) {
            Payload replacement = new Payload(states + 20);
            box.value = replacement;
            identity(seen.value, replacement);
        }
        states++;
    }
    public static void main(String[] args) throws Throwable {
        OriginalRawParametersHarness.main(args);
        if (Api.class.getTypeParameters().length != 0 || Mark.class.getTypeParameters().length != 0)
            throw new AssertionError("dependency owner acquired a class formal");
        Method sdk = Api.class.getDeclaredMethod("select", RawBox.class);
        Method inherited = Api.class.getDeclaredMethod("inherited", RawBox.class);
        formal(sdk, "M"); formal(inherited, "M");
        formal(Forwarder.class.getDeclaredMethod("select", RawBox.class), "N");
        formal(Parent.class.getDeclaredMethod("select", RawBox.class), "N");
        if (!inherited.isDefault() || Leaf.class.getMethod("select", RawBox.class).getDeclaringClass() != Parent.class
                || Forwarder.class.getMethod("inherited", RawBox.class).getDeclaringClass() != Api.class)
            throw new AssertionError("original generic provider or default ownership changed");
        for (Class<?> owner : new Class<?>[] {Forwarder.class, Parent.class, Leaf.class})
            for (Method method : owner.getDeclaredMethods())
                if (method.isBridge()) throw new AssertionError("SDK method proof invented a bridge");
        Call[] calls = {
            (r, b) -> r instanceof Forwarder ? ((Forwarder) r).select(b) : ((Parent) r).select(b),
            (r, b) -> r.select(b),
            (r, b) -> (Mark) invoke(sdk, r, b),
            (r, b) -> (Mark) invoke(r.getClass().getMethod("select", RawBox.class), r, b),
            (r, b) -> r.inherited(b),
            (r, b) -> (Mark) invoke(inherited, r, b)
        };
        Sink sink = new Sink();
        Api[] receivers = {new Forwarder(sink), new Leaf(), new Overriding()};
        @SuppressWarnings("unchecked")
        RawBox<Payload>[] inputs = new RawBox[] {null, new RawBox<Payload>(null),
            new RawBox<Payload>(new Payload(1)), new RawBox<Payload>(new Payload(2))};
        for (Api receiver : receivers) for (RawBox<Payload> box : inputs)
            for (int route = 0; route < calls.length; route++) {
                int before = count(receiver);
                identity(calls[route].run(receiver, box), box == null ? null : box.value);
                state(receiver, box, before, route);
            }
        RuntimeException problem = new IllegalStateException("owned method-formal failure");
        sink.failure = problem;
        for (Api receiver : receivers) {
            if (receiver instanceof Parent) ((Parent) receiver).failure = problem;
            for (int route = 0; route < calls.length; route++) {
                int before = count(receiver);
                try { calls[route].run(receiver, inputs[1]); throw new AssertionError("generic provider failure missing"); }
                catch (RuntimeException actual) { identity(actual, problem); failures++; }
                state(receiver, inputs[1], before, route);
            }
            if (receiver instanceof Parent) ((Parent) receiver).failure = null;
        }
        sink.failure = null;
        for (int route = 0; route < calls.length; route++) {
            try { calls[route].run(null, null); throw new AssertionError("null generic receiver accepted"); }
            catch (NullPointerException expected) { failures++; }
            if (Trace.events != 0) throw new AssertionError("null receiver acquired an effect");
            try { calls[route].run(new Forwarder(null), null); throw new AssertionError("null generic capture accepted"); }
            catch (NullPointerException expected) { failures++; }
            if (Trace.events != (route >= 4 ? 64 : 4)) throw new AssertionError("null capture failure order changed");
            Trace.events = 0;
        }
        System.out.println("dependency-method-formal-identities=" + identities + ",states=" + states
                + ",failures=" + failures + ",bridges=0");
        System.out.println("complete-sdk-method-scopes-and-default-ownership=retained,virtual-dispatch-and-mutable-aliases=matched");
    }
}
