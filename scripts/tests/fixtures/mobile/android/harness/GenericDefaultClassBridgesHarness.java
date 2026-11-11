import fixture.*;
import java.lang.reflect.*;

public final class GenericDefaultClassBridgesHarness {
    private static int identities, states, failures;
    private interface Call { Object run() throws Throwable; }
    private static final class Sink implements Callback<String, byte[]> {
        int calls;
        String key;
        byte[] value;
        RuntimeException failure;
        public void removed(String nextKey, byte[] nextValue) {
            calls++; key = nextKey; value = nextValue;
            Trace.events = Trace.events * 10 + 4;
            if (failure != null) throw failure;
        }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original listener or capture identity changed");
        identities++;
    }
    private static Object reflect(Method method, Object receiver, Object... args) throws Throwable {
        try { return method.invoke(receiver, args); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static Method listener(Class<?> owner, Class<?> result) {
        for (Method method : owner.getDeclaredMethods()) {
            if (method.getName().equals("listener") && method.getReturnType() == result
                    && java.util.Arrays.equals(method.getParameterTypes(), new Class<?>[] {Callback.class}))
                return method;
        }
        throw new AssertionError("missing original listener return entry");
    }
    private static Call[] listenerPaths(Leaf<String, byte[]> leaf, Callback<String, byte[]> callback,
                                        Method typed, Method bridge, Method contract) {
        Base<String, byte[]> base = leaf;
        Middle<String, byte[]> middle = leaf;
        Contract<String, byte[]> api = leaf;
        return new Call[] {
            () -> base.listener(callback), () -> middle.listener(callback), () -> leaf.listener(callback),
            () -> api.listener(callback), () -> reflect(typed, leaf, callback),
            () -> reflect(bridge, leaf, callback), () -> reflect(contract, leaf, callback)
        };
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Call[] firePaths(Leaf<String, byte[]> leaf, Object key, Object value, Method fire) {
        Base base = leaf; Middle middle = leaf; Leaf raw = leaf;
        return new Call[] {
            () -> { base.fire(key, value); return null; },
            () -> { middle.fire(key, value); return null; },
            () -> { raw.fire(key, value); return null; },
            () -> reflect(fire, leaf, key, value)
        };
    }
    private static void fail(Call call, Class<? extends Throwable> kind, RuntimeException same) {
        try { call.run(); throw new AssertionError("failure missing"); }
        catch (Throwable error) {
            if (error.getClass() != kind || (same != null && error != same))
                throw new AssertionError("original exception type or identity changed", error);
            if (same != null) identities++;
            failures++;
        }
    }
    private static void listen(Call call, Leaf<String, byte[]> leaf, Callback<String, byte[]> callback,
                               int delta, RuntimeException problem, boolean absent) throws Throwable {
        int before = leaf.listeners;
        Callback<String, byte[]> previous = leaf.current;
        Trace.events = 0;
        if (absent) fail(call, NullPointerException.class, null);
        else if (problem != null) fail(call, IllegalStateException.class, problem);
        else same(call.run(), leaf);
        if (leaf.listeners != before + delta || Trace.events != delta || Trace.defaultCalls != 0)
            throw new AssertionError("default body ran or class listener effects changed");
        same(leaf.current, delta == 0 ? previous : callback);
        states++;
    }
    private static void fire(Call call, Leaf<String, byte[]> leaf, Owner owner, Sink sink,
                             Object key, Object value, int baseDelta, int ownerDelta, int sinkDelta,
                             int events, Class<? extends Throwable> kind, RuntimeException problem) throws Throwable {
        int baseCalls = leaf.fires, ownerCalls = owner.removals, sinkCalls = sink.calls;
        String oldKey = owner.key, oldSinkKey = sink.key;
        byte[] oldValue = owner.value, oldSinkValue = sink.value;
        Trace.events = 0;
        if (kind != null) fail(call, kind, problem);
        else if (call.run() != null) throw new AssertionError("void callback returned a value");
        if (leaf.fires != baseCalls + baseDelta || owner.removals != ownerCalls + ownerDelta
                || sink.calls != sinkCalls + sinkDelta || Trace.events != events || Trace.defaultCalls != 0)
            throw new AssertionError("capture, bridge cast or callback effect order changed");
        same(owner.key, ownerDelta == 0 ? oldKey : key);
        same(owner.value, ownerDelta == 0 ? oldValue : value);
        same(sink.key, sinkDelta == 0 ? oldSinkKey : key);
        same(sink.value, sinkDelta == 0 ? oldSinkValue : value);
        states++;
    }
    private static void genericListener(Method method, Class<?> result, Class<?> scope) {
        Type returned = method.getGenericReturnType();
        Type parameter = method.getGenericParameterTypes()[0];
        if (!(returned instanceof ParameterizedType) || !(parameter instanceof ParameterizedType)
                || ((ParameterizedType) returned).getRawType() != result
                || ((ParameterizedType) parameter).getRawType() != Callback.class
                || !java.util.Arrays.equals(((ParameterizedType) returned).getActualTypeArguments(), scope.getTypeParameters())
                || !java.util.Arrays.equals(((ParameterizedType) parameter).getActualTypeArguments(), scope.getTypeParameters()))
            throw new AssertionError("complete listener parameter or return scope changed");
    }
    public static void main(String[] args) throws Throwable {
        boolean original = args[0].equals("original");
        RuntimeException problem = new IllegalStateException("owned generic default failure");
        Trace.defaultFailure = problem;
        Trace.events = 0; Trace.defaultCalls = 0;
        Contract<String, byte[]> defaultControl = new Contract<String, byte[]>() {};
        fail(() -> defaultControl.listener(null), IllegalStateException.class, problem);
        if (Trace.defaultCalls != 1 || Trace.events != 9) throw new AssertionError("default control did not execute");
        states++;
        Trace.defaultCalls = 0;
        Owner owner = new Owner();
        Leaf<String, byte[]> leaf = owner.create();
        Method typed = listener(Base.class, Base.class);
        Method bridge = listener(Base.class, Contract.class);
        Method contract = listener(Contract.class, Contract.class);
        if (typed.isBridge() || !bridge.isBridge() || !bridge.isSynthetic() || !contract.isDefault())
            throw new AssertionError("original class bridge or interface default flags changed");
        genericListener(typed, Base.class, Base.class);
        genericListener(contract, Contract.class, Contract.class);
        if (!Middle.class.getGenericSuperclass().getTypeName().equals("fixture.Base<A, B>")
                || !Leaf.class.getGenericSuperclass().getTypeName().equals("fixture.Middle<A, B>")
                || !leaf.getClass().getGenericSuperclass().getTypeName().equals("fixture.Leaf<java.lang.String, byte[]>"))
            throw new AssertionError("composed class argument scopes changed");
        for (Class<?> current : new Class<?>[] {Middle.class, Leaf.class, leaf.getClass()}) {
            for (Method method : current.getDeclaredMethods()) {
                if (method.getName().equals("listener"))
                    throw new AssertionError("inherited class listener was copied into a descendant");
            }
        }
        if (leaf.getClass().isAnonymousClass() != original || leaf.getClass().isLocalClass() == original
                || !leaf.getClass().getEnclosingMethod().equals(Owner.class.getDeclaredMethod("create")))
            throw new AssertionError("reported anonymous class reflection changed");
        Field captured = leaf.getClass().getDeclaredField("this$0");
        captured.setAccessible(true);
        same(captured.get(leaf), owner);
        Sink sink = new Sink(); Sink other = new Sink();
        for (Callback<String, byte[]> callback : java.util.Arrays.asList(sink, other, null)) {
            for (Call call : listenerPaths(leaf, callback, typed, bridge, contract))
                listen(call, leaf, callback, 1, null, false);
        }
        leaf.failure = problem;
        for (Call call : listenerPaths(leaf, sink, typed, bridge, contract))
            listen(call, leaf, sink, 1, problem, false);
        leaf.failure = null;
        for (Call call : listenerPaths(null, sink, typed, bridge, contract))
            listen(call, leaf, sink, 0, null, true);
        Method fireMethod = Base.class.getDeclaredMethod("fire", Object.class, Object.class);
        String[] keys = {new String("a"), null, new String("b")};
        byte[][] values = {new byte[0], null, new byte[] {1, 2}};
        for (int i = 0; i < keys.length; i++) {
            for (Call call : firePaths(leaf, keys[i], values[i], fireMethod))
                fire(call, leaf, owner, sink, keys[i], values[i], 1, 1, 1, 234, null, null);
        }
        owner.failure = problem;
        for (Call call : firePaths(leaf, keys[0], values[0], fireMethod))
            fire(call, leaf, owner, sink, keys[0], values[0], 1, 1, 0, 23, IllegalStateException.class, problem);
        owner.failure = null; sink.failure = problem;
        for (Call call : firePaths(leaf, keys[0], values[0], fireMethod))
            fire(call, leaf, owner, sink, keys[0], values[0], 1, 1, 1, 234, IllegalStateException.class, problem);
        sink.failure = null;
        for (Object[] invalid : new Object[][] {{new Object(), values[0]}, {keys[0], new Object()}}) {
            for (Call call : firePaths(leaf, invalid[0], invalid[1], fireMethod))
                fire(call, leaf, owner, sink, invalid[0], invalid[1], 1, 0, 0, 2, ClassCastException.class, null);
        }
        for (Call call : firePaths(null, keys[0], values[0], fireMethod))
            fire(call, leaf, owner, sink, keys[0], values[0], 0, 0, 0, 0, NullPointerException.class, null);
        System.out.println("identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("ancestor-class-bridge=retained,default-never-dispatched-for-class=matched,captured-callback-order=matched");
        System.out.println(original ? "class-reflection=original-anonymous" : "class-reflection=reported-named-local");
    }
}
