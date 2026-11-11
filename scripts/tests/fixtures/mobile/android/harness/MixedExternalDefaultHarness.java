import fixture.*;
import java.lang.reflect.*;

public final class MixedExternalDefaultHarness {
    private static int identities, failures;
    private static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    private static void same(Object a, Object b) { check(a == b); identities++; }
    private static final class Derived extends Receiver<Object,Object> {
        public Object get(Object key) { calls += 2; Trace.mark(3); return value; }
    }
    private static Object call(Receiver<Object,Object> receiver, Object key, Object fallback, int mode) {
        if (mode == 0) return receiver.getOrDefault(key, fallback);
        if (mode == 1) return ((Root<Object>) receiver).getOrDefault(key, fallback);
        if (mode == 2) return ((Child<Object>) receiver).getOrDefault(key, fallback);
        return ((Middle<Object,Object>) receiver).getOrDefault(key, fallback);
    }
    public static void main(String[] args) throws Exception {
        Receiver<Object,Object> receiver = new Receiver<>();
        Object first = new Object(), second = new Object();
        Object[] values = {first, second, null};
        for (Object value : values) for (Object fallback : values) for (int mode = 0; mode < 4; mode++) {
            receiver.value = value;
            Trace.events = 0;
            int before = receiver.calls;
            same(call(receiver, mode == 0 ? null : first, fallback, mode), value != null ? value : fallback);
            check(Trace.events == 12 && receiver.calls == before + 1);
        }
        Derived derived = new Derived(); derived.value = second;
        for (int mode = 0; mode < 4; mode++) {
            Trace.events = 0;
            int before = derived.calls;
            same(call(derived, null, first, mode), second);
            check(Trace.events == 13 && derived.calls == before + 2);
        }
        RuntimeException sentinel = new IllegalStateException("owned");
        receiver.failure = sentinel;
        for (int mode = 0; mode < 4; mode++) {
            Trace.events = 0;
            int before = receiver.calls;
            try { call(receiver, first, second, mode); throw new AssertionError(); }
            catch (RuntimeException actual) { same(actual, sentinel); failures++; }
            check(Trace.events == 12 && receiver.calls == before + 1);
        }
        check(Child.class.getDeclaredMethods().length == 0);
        check(Middle.class.getDeclaredMethods().length == 0);
        check(Leaf.class.getDeclaredMethods().length == 0);
        Method method = Receiver.class.getMethod("getOrDefault", Object.class, Object.class);
        check(method.getDeclaringClass() == Root.class && method.isDefault() && !method.isBridge());
        check(method.getGenericReturnType().getTypeName().equals("T"));
        check(Middle.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Child<V>"));
        check(Leaf.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Middle<Q, P>"));
        check(Leaf.class.getGenericInterfaces()[1].getTypeName().equals("fixture.Child<P>"));
        check(Receiver.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Leaf<S, V>"));
        check(Receiver.class.getDeclaredMethod("get", Object.class).getGenericReturnType().getTypeName().equals("S"));
        if (args.length != 0 && args[0].equals("ancestor")) {
            Class<?> parent = Class.forName("fixture.Parent");
            check(Receiver.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<V, S>"));
            check(parent.getGenericInterfaces()[0].getTypeName().equals("fixture.Child<R>"));
            check(parent.getMethod("getOrDefault",Object.class,Object.class).getDeclaringClass() == Root.class);
        }
        System.out.println("identities=" + identities + ",failures=" + failures);
        System.out.println("external-default-owner=retained,full-source-edges=retained,dispatch-and-effects=matched");
    }
}
