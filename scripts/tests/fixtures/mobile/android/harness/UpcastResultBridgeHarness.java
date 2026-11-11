import fixture.*;
import java.lang.reflect.*;

public final class UpcastResultBridgeHarness {
    private static int identities, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("returned identity changed");
        identities++;
    }
    private static void events(int expected) {
        if (Trace.events != expected) throw new AssertionError("effects changed: " + Trace.events);
        Trace.events = 0;
    }
    private static void failure(Runnable call, Class<? extends Throwable> kind,
                                RuntimeException same, int effects) {
        try { call.run(); throw new AssertionError("failure missing"); }
        catch (Throwable e) {
            if (e.getClass() != kind || (same != null && e != same))
                throw new AssertionError("failure identity or type changed", e);
            if (same != null) identities++;
            failures++;
        }
        events(effects);
    }
    private static final class Overriding extends Receiver {
        public Leaf create() { Trace.events = Trace.events * 10 + 5; return value; }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Receiver receiver = new Receiver();
        Parent<Leaf> parent = receiver;
        Parent raw = receiver;
        Leaf[] values = {new Leaf(), new Leaf(), null};
        for (Leaf value : values) {
            receiver.value = value;
            identity(receiver.create(), value); events(1);
            identity(parent.create(), value); events(1);
            identity(raw.create(), value); events(1);
            identity(Receiver.checked(value), value); events(0);
        }
        Overriding override = new Overriding(); override.value = values[0];
        Receiver receiverView = override;
        Parent<Leaf> parentView = override;
        Parent rawView = override;
        identity(receiverView.create(), values[0]); events(5);
        identity(parentView.create(), values[0]); events(5);
        identity(rawView.create(), values[0]); events(5);
        RuntimeException problem = new IllegalStateException("owned target failure");
        Trace.targetFailure = problem;
        failure(() -> receiver.create(), IllegalStateException.class, problem, 1);
        failure(() -> parent.create(), IllegalStateException.class, problem, 1);
        failure(() -> raw.create(), IllegalStateException.class, problem, 1);
        identity(parentView.create(), values[0]); events(5);
        Trace.targetFailure = null;
        Receiver absent = null;
        Parent<Leaf> absentParent = null;
        Parent absentRaw = null;
        failure(() -> absent.create(), NullPointerException.class, null, 0);
        failure(() -> absentParent.create(), NullPointerException.class, null, 0);
        failure(() -> absentRaw.create(), NullPointerException.class, null, 0);
        failure(() -> Receiver.checked(new Object()), ClassCastException.class, null, 0);
        int bridges = 0;
        for (Method method : Receiver.class.getDeclaredMethods()) {
            if (method.getName().equals("create") && method.getReturnType() == Base.class) {
                if (!method.isBridge() || !method.isSynthetic())
                    throw new AssertionError("original bridge flags changed");
                bridges++;
            }
        }
        if (bridges != 1 || Receiver.class.getMethod("create").getReturnType() != Leaf.class
                || !Receiver.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<fixture.Leaf>")
                || !Parent.class.getMethod("create").getGenericReturnType().equals(Parent.class.getTypeParameters()[0])
                || !Parent.class.getTypeParameters()[0].getBounds()[0].equals(Base.class))
            throw new AssertionError("full generic result or bound changed");
        System.out.println("identities=" + identities + ",failures=" + failures);
        System.out.println("result-upcast-and-virtual-dispatch=matched,generic-reflection=unchanged");
    }
}
