import fixture.*;
import kotlin.Unit;
import java.lang.reflect.*;

public final class BooleanUnitBridgeHarness {
    private static int identities, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("identity changed");
        identities++;
    }
    private static void events(int expected) {
        if (Trace.events != expected) throw new AssertionError("effect order changed");
    }
    private static void fields(Receiver<Object,Object> r, Object k, Object a, Object b, boolean flag) {
        identity(r.key, k); identity(r.previous, a); identity(r.replacement, b);
        if (r.flag != flag) throw new AssertionError("boolean changed");
    }
    private static void failure(Runnable action, Class<? extends Throwable> expected) {
        try { action.run(); throw new AssertionError("failure missing"); }
        catch (Throwable e) {
            if (e.getClass() != expected) throw new AssertionError("failure type changed", e);
            failures++;
        }
    }
    public static void main(String[] args) throws Exception {
        Receiver<Object,Object> r = new Receiver<>();
        Object key = new Object(), a = new Object(), b = new Object();
        events(0);
        r.invoke(true, key, a, b);
        events(1); fields(r, key, a, b, true);
        RuntimeException problem = new IllegalStateException("owned callback failure");
        r.failure = problem;
        try { r.invoke(false, null, null, null); throw new AssertionError("typed failure missing"); }
        catch (IllegalStateException e) { identity(e, problem); failures++; }
        events(11); fields(r, null, null, null, false);
        r.failure = null; Trace.events = 0;
        failure(() -> r.invoke((Object) Integer.valueOf(1), key, a, b), ClassCastException.class);
        events(0);
        failure(() -> r.invoke((Object) null, key, a, b), NullPointerException.class);
        events(0);
        Object result = r.invoke((Object) Boolean.TRUE, key, a, b);
        events(12); fields(r, key, a, b, true); identity(result, Unit.INSTANCE);
        result = r.invoke((Object) Boolean.FALSE, a, b, key);
        events(121); fields(r, a, b, key, false); identity(result, Unit.INSTANCE);
        r.failure = problem;
        try { r.invoke((Object) Boolean.TRUE, key, a, b); throw new AssertionError("bridge failure missing"); }
        catch (IllegalStateException e) { identity(e, problem); failures++; }
        events(1211); fields(r, key, a, b, true);
        Function raw = r;
        failure(() -> raw.invoke("incompatible", key, a, b), ClassCastException.class);
        events(1211);
        Receiver<Object,Object> absent = null;
        failure(() -> absent.invoke((Object) null, key, a, b), NullPointerException.class);
        failure(() -> absent.invoke(false, key, a, b), NullPointerException.class);
        events(1211);
        r.failure = null;
        @SuppressWarnings("unchecked") Function<Boolean,Object,Object,Object,Unit> view = r;
        identity(view.invoke(Boolean.FALSE, key, a, b), Unit.INSTANCE);
        events(12111);
        boolean recovered = args[0].equals("recovered");
        Method bridge = Receiver.class.getMethod("invoke", Object.class, Object.class, Object.class, Object.class);
        Method typed = Receiver.class.getMethod("invoke", boolean.class, Object.class, Object.class, Object.class);
        if (bridge.isBridge() == recovered || bridge.isSynthetic() == recovered)
            throw new AssertionError("bridge reflection differs from receipt");
        Type[] formals = Receiver.class.getTypeParameters();
        Type[] parameters = typed.getGenericParameterTypes();
        if (!parameters[0].equals(boolean.class) || !parameters[1].equals(formals[0])
                || !parameters[2].equals(formals[1]) || !parameters[3].equals(formals[1]))
            throw new AssertionError("typed formal metadata changed");
        Type api = Receiver.class.getGenericInterfaces()[0];
        if (recovered ? !api.equals(Function.class) : !(api instanceof ParameterizedType))
            throw new AssertionError("interface reflection differs from receipt");
        System.out.println("identities=" + identities + ",failures=" + failures + ",boolean-dispatch=matched");
        System.out.println(recovered ? "reflection=reported-raw-interface" : "reflection=original");
    }
}
