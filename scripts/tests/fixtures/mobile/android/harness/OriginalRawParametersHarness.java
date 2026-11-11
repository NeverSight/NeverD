import fixture.*;
import java.lang.reflect.*;

public final class OriginalRawParametersHarness {
    private static int identities, states, failures;
    private interface Call {
        void run(Api receiver, RawBox<?> box, Object tag) throws Throwable;
    }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("reference identity changed");
        identities++;
    }
    private static void events(int expected) {
        if (Trace.events != expected) throw new AssertionError("effect order changed: " + Trace.events);
        Trace.events = 0;
    }
    private static final class Overriding extends Leaf {
        @SuppressWarnings("rawtypes")
        public void accept(RawBox box, Object tag) {
            Trace.events = Trace.events * 10 + 5;
            seen = box; this.tag = tag; calls++;
            if (failure != null) throw failure;
        }
    }
    private static int count(Api receiver) {
        return receiver instanceof Forwarder ? ((Forwarder) receiver).sink.calls : ((Parent) receiver).calls;
    }
    private static void state(Api receiver, int before, RawBox<?> box, Object tag) {
        RawBox<?> seen; Object stored;
        if (receiver instanceof Forwarder) {
            Sink sink = ((Forwarder) receiver).sink;
            seen = sink.seen; stored = sink.tag;
        } else {
            Parent parent = (Parent) receiver;
            seen = parent.seen; stored = parent.tag;
        }
        if (count(receiver) != before + 1) throw new AssertionError("state update count changed");
        identity(seen, box); identity(stored, tag);
        if (box != null) {
            Object value = new Object();
            ((RawBox) box).value = value;
            identity(seen.value, value);
        }
        states++;
        events(receiver instanceof Forwarder ? 31 : receiver instanceof Overriding ? 5 : 2);
    }
    private static void invoke(Method method, Object receiver, Object... arguments) throws Throwable {
        try { method.invoke(receiver, arguments); }
        catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        Method api = Api.class.getMethod("accept", RawBox.class, Object.class);
        ParameterizedType parameter = (ParameterizedType) api.getGenericParameterTypes()[0];
        if (parameter.getRawType() != RawBox.class
                || !(((WildcardType) parameter.getActualTypeArguments()[0]).getUpperBounds()[0] == Object.class))
            throw new AssertionError("complete wildcard interface contract changed");
        for (Class<?> owner : new Class<?>[] {Forwarder.class, Parent.class}) {
            Method method = owner.getDeclaredMethod("accept", RawBox.class, Object.class);
            if (method.getGenericParameterTypes()[0] != RawBox.class || method.isBridge()
                    || method.getTypeParameters().length != 0 || method.getExceptionTypes().length != 0)
                throw new AssertionError("original raw parameter acquired a signature or bridge");
        }
        for (Method method : Leaf.class.getDeclaredMethods())
            if (!Modifier.isPrivate(method.getModifiers()))
                throw new AssertionError("inherited implementation was copied");
        if (Leaf.class.getMethod("accept", RawBox.class, Object.class).getDeclaringClass() != Parent.class)
            throw new AssertionError("inherited implementation was copied");
        Call[] calls = {
            (r, b, t) -> { if (r instanceof Forwarder) ((Forwarder) r).accept(b, t); else ((Parent) r).accept(b, t); },
            (r, b, t) -> r.accept(b, t),
            (r, b, t) -> invoke(api, r, b, t),
            (r, b, t) -> invoke(r.getClass().getMethod("accept", RawBox.class, Object.class), r, b, t)
        };
        Sink sink = new Sink();
        Api[] receivers = {new Forwarder(sink), new Leaf(), new Overriding()};
        RawBox<?>[] inputs = {null, new RawBox<>(null), new RawBox<>("owned"), new RawBox<>(Integer.valueOf(7))};
        Object[] tags = {null, "tag", new Object()};
        for (Api receiver : receivers) for (RawBox<?> box : inputs) for (Object tag : tags)
            for (Call call : calls) {
                int before = count(receiver);
                call.run(receiver, box, tag);
                state(receiver, before, box, tag);
            }
        RuntimeException problem = new IllegalStateException("owned raw parameter failure");
        sink.failure = problem;
        for (Api receiver : receivers) {
            if (receiver instanceof Parent) ((Parent) receiver).failure = problem;
            for (Call call : calls) {
                int before = count(receiver);
                try { call.run(receiver, inputs[1], tags[2]); throw new AssertionError("failure missing"); }
                catch (RuntimeException actual) { identity(actual, problem); failures++; }
                state(receiver, before, inputs[1], tags[2]);
            }
            if (receiver instanceof Parent) ((Parent) receiver).failure = null;
        }
        sink.failure = null;
        for (Call call : calls) {
            try { call.run(null, null, null); throw new AssertionError("null receiver accepted"); }
            catch (NullPointerException expected) { failures++; }
            events(0);
            try { call.run(new Forwarder(null), null, null); throw new AssertionError("null capture accepted"); }
            catch (NullPointerException expected) { failures++; }
            events(3);
        }
        System.out.println("values=36,identities=" + identities + ",states=" + states + ",failures=" + failures + ",bridges=0");
        System.out.println("original-raw-parameters-and-default-free-ownership=retained,capture-and-inherited-dispatch=matched");
    }
}
