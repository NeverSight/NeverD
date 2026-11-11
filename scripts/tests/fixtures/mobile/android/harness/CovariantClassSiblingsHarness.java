import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class CovariantClassSiblingsHarness {
    private static int identities, states, failures;
    private interface Call { Broad run(Forwarder receiver, Object marker) throws Throwable; }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("sibling result alias changed");
        identities++;
    }
    private static Method entry(Class<?> owner, String name, Class<?> result, Class<?>... parameters) throws Exception {
        for (Method method : owner.getDeclaredMethods())
            if (method.getName().equals(name) && method.getReturnType() == result
                    && java.util.Arrays.equals(method.getParameterTypes(), parameters)) {
                method.setAccessible(true);
                return method;
            }
        throw new AssertionError("original sibling entry missing");
    }
    private static Object invoke(Method method, Object receiver, Object... parameters) throws Throwable {
        try { return method.invoke(receiver, parameters); }
        catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    private static final class Overriding extends Forwarder {
        Overriding() { super(new Sink()); }
        public Narrow makeValue(Object marker) { touches += 3; return super.makeValue(marker); }
        protected Narrow defaultValue() { touches += 3; return super.defaultValue(); }
    }
    private static void state(Forwarder receiver, int before, Object marker, boolean changesMarker) {
        if (receiver.touches != before + (receiver instanceof Overriding ? 4 : 1)
                || (changesMarker && receiver.seen != marker))
            throw new AssertionError("sibling dispatch changed effects before result or failure");
        states++;
    }
    public static void main(String[] args) throws Throwable {
        PlainClassCovarianceHarness.main(new String[]{"siblings"});
        if (Value.class.getSuperclass() != Broad.class || AbstractWidget.class.getSuperclass() != Root.class)
            throw new AssertionError("original nominal ancestry changed");
        Method rootMake = entry(Root.class, "makeValue", Broad.class, Object.class);
        Method broadMake = entry(Forwarder.class, "makeValue", Broad.class, Object.class);
        Method rootDefault = entry(Root.class, "defaultValue", Broad.class);
        Method broadDefault = entry(Forwarder.class, "defaultValue", Broad.class);
        for (Method bridge : new Method[]{broadMake, broadDefault})
            if (!bridge.isBridge() || !bridge.isSynthetic() || bridge.getTypeParameters().length != 0
                    || bridge.getExceptionTypes().length != 0 || bridge.getGenericReturnType() != Broad.class)
                throw new AssertionError("sibling bridge complete contract changed");
        if (!Modifier.isPublic(broadMake.getModifiers()) || !Modifier.isProtected(broadDefault.getModifiers()))
            throw new AssertionError("sibling visibility changed");
        MethodHandles.Lookup lookup = MethodHandles.lookup();
        MethodHandle rootMakeHandle = lookup.unreflect(rootMake);
        MethodHandle broadMakeHandle = lookup.unreflect(broadMake);
        MethodHandle rootDefaultHandle = lookup.unreflect(rootDefault);
        MethodHandle broadDefaultHandle = lookup.unreflect(broadDefault);
        Call[] calls = {
            (r, m) -> (Broad) invoke(rootMake, r, m),
            (r, m) -> (Broad) invoke(broadMake, r, m),
            (r, m) -> (Broad) rootMakeHandle.invoke(r, m),
            (r, m) -> (Broad) broadMakeHandle.invoke(r, m),
            (r, m) -> (Broad) invoke(rootDefault, r),
            (r, m) -> (Broad) invoke(broadDefault, r),
            (r, m) -> (Broad) rootDefaultHandle.invoke(r),
            (r, m) -> (Broad) broadDefaultHandle.invoke(r)
        };
        Forwarder[] receivers = {new Forwarder(new Sink()), new Overriding()};
        for (Forwarder receiver : receivers) for (Object marker : new Object[]{null, "owned", new Object()})
            for (int route = 0; route < calls.length; route++) {
                int before = receiver.touches;
                Broad result = calls[route].run(receiver, marker);
                identity(result, receiver.value);
                ((Value) result).tag = marker;
                identity(receiver.value.tag, marker);
                state(receiver, before, marker, route < 4);
            }
        RuntimeException problem = new IllegalStateException("owned sibling failure");
        for (Forwarder receiver : receivers) {
            receiver.failure = problem;
            for (int route = 0; route < calls.length; route++) {
                int before = receiver.touches;
                try { calls[route].run(receiver, problem); throw new AssertionError("original failure missing"); }
                catch (RuntimeException actual) { identity(actual, problem); failures++; }
                state(receiver, before, problem, route < 4);
            }
            receiver.failure = null;
        }
        for (Call call : calls) {
            try { call.run(null, problem); throw new AssertionError("null receiver accepted"); }
            catch (NullPointerException expected) { failures++; }
        }
        System.out.println("sibling-class-covariant-values=48,identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-sibling-bridges-and-full-metadata=retained,root-and-subclass-dispatch=matched");
    }
}
