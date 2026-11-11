import fixture.*;
import java.lang.reflect.*;

public final class CovariantReferenceGetterHarness {
    private static int identities, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("covariant getter identity changed");
        identities++;
    }
    private static void events(int expected) {
        if (Trace.events != expected) throw new AssertionError("getter dispatch changed");
        Trace.events = 0;
    }
    private static void failure(Runnable call, RuntimeException same, int expected) {
        try { call.run(); throw new AssertionError("failure missing"); }
        catch (RuntimeException e) {
            if (same != null) identity(e,same);
            else if (e.getClass() != NullPointerException.class) throw new AssertionError(e);
            failures++;
        }
        events(expected);
    }
    private static final class Override extends Receiver<Object,Object> {
        public Leaf<Object> create() { Trace.events = Trace.events * 10 + 2; return value; }
    }
    public static void main(String[] args) throws Exception {
        Receiver<Object,Object> receiver = new Receiver<>(); Parent<Object> base = receiver;
        Method typed = Receiver.class.getMethod("create");
        Method bridge = null;
        for (Method method : Receiver.class.getDeclaredMethods())
            if (method.getName().equals("create") && method.getReturnType() == Base.class) bridge = method;
        if (bridge == null || !bridge.isBridge() || !bridge.isSynthetic())
            throw new AssertionError("original bridge flags changed");
        for (Leaf<Object> value : new Leaf[]{new Leaf<Object>(new Object()),new Leaf<Object>(null),null}) {
            receiver.value = value;
            identity(receiver.create(),value); events(1);
            identity(base.create(),value); events(1);
            identity(typed.invoke(receiver),value); events(1);
            identity(bridge.invoke(receiver),value); events(1);
        }
        Override child = new Override(); child.value = new Leaf<>("child");
        Parent<Object> childBase = child;
        identity(childBase.create(),child.value); events(2);
        identity(bridge.invoke(child),child.value); events(2);
        Object payload = new Object(); child.value.value = payload;
        identity(((Leaf<Object>) childBase.create()).value,payload); events(2);
        RuntimeException problem = new IllegalStateException("owned getter failure");
        Trace.targetFailure = problem;
        failure(() -> receiver.create(),problem,1);
        failure(() -> base.create(),problem,1);
        try { bridge.invoke(receiver); throw new AssertionError("reflective failure missing"); }
        catch (InvocationTargetException e) { identity(e.getCause(),problem); failures++; }
        events(1); Trace.targetFailure = null;
        Receiver absent = null; Parent absentBase = null;
        failure(() -> absent.create(),null,0); failure(() -> absentBase.create(),null,0);
        Type s = Receiver.class.getTypeParameters()[0];
        Type returned = ((ParameterizedType) typed.getGenericReturnType()).getActualTypeArguments()[0];
        Type inherited = ((ParameterizedType) Receiver.class.getGenericSuperclass()).getActualTypeArguments()[0];
        if (!s.equals(returned) || !s.equals(inherited)
                || !((ParameterizedType) Parent.class.getMethod("create").getGenericReturnType())
                    .getActualTypeArguments()[0].equals(Parent.class.getTypeParameters()[0]))
            throw new AssertionError("complete getter scope changed");
        System.out.println("identities=" + identities + ",failures=" + failures);
        System.out.println("full-generic-covariance=retained,original-upcast-and-dispatch=matched");
    }
}
