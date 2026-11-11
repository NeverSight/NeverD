import fixture.*;
import java.lang.reflect.*;

public final class PlainClassCovarianceHarness {
    private static int identities, states, failures;
    private interface Call { Value run(Forwarder receiver, Object marker) throws Throwable; }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("covariant class alias changed");
        identities++;
    }
    private static Object invoke(Method method, Object receiver, Object... arguments) throws Throwable {
        try { return method.invoke(receiver, arguments); }
        catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    private static Method entry(String name, Class<?> result, Class<?>... arguments) throws Exception {
        for (Method method : Forwarder.class.getDeclaredMethods())
            if (method.getName().equals(name) && method.getReturnType() == result
                    && java.util.Arrays.equals(method.getParameterTypes(), arguments)) {
                method.setAccessible(true);
                return method;
            }
        throw new AssertionError("original covariant runtime entry missing");
    }
    private static final class Overriding extends Forwarder {
        public Overriding() { super(new Sink()); }
        public Narrow makeValue(Object marker) {
            touches += 3;
            return super.makeValue(marker);
        }
        protected Narrow defaultValue() {
            touches += 3;
            return super.defaultValue();
        }
    }
    private static void state(Forwarder receiver, int before, Object marker, boolean updates) {
        int amount = receiver instanceof Overriding ? 4 : 1;
        if (receiver.stamp != 7 || receiver.touches != before + amount
                || (updates && receiver.seen != marker))
            throw new AssertionError("original bridge virtual effects changed");
        states++;
    }
    public static void main(String[] args) throws Throwable {
        OriginalRawParametersHarness.main(args);
        if (Forwarder.class.getSuperclass() != AbstractWidget.class
                || Forwarder.class.getTypeParameters().length != 0
                || Narrow.class.getSuperclass() != Value.class)
            throw new AssertionError("plain source or result class ancestry changed");
        Method sdkMake = AbstractWidget.class.getDeclaredMethod("makeValue", Object.class);
        Method sdkDefault = AbstractWidget.class.getDeclaredMethod("defaultValue");
        sdkDefault.setAccessible(true);
        Method typedMake = entry("makeValue", Narrow.class, Object.class);
        Method broadMake = entry("makeValue", Value.class, Object.class);
        Method typedDefault = entry("defaultValue", Narrow.class);
        Method broadDefault = entry("defaultValue", Value.class);
        if (typedMake.isBridge() || typedDefault.isBridge()
                || !broadMake.isBridge() || !broadDefault.isBridge()
                || !Modifier.isPublic(broadMake.getModifiers())
                || !Modifier.isProtected(broadDefault.getModifiers())
                || typedMake.getGenericParameterTypes()[0] != Object.class
                || typedMake.getGenericReturnType() != Narrow.class
                || typedDefault.getGenericReturnType() != Narrow.class)
            throw new AssertionError("original public/protected bridge metadata changed");
        int bridges = 0;
        for (Method method : Forwarder.class.getDeclaredMethods()) if (method.isBridge()) bridges++;
        if (bridges != (args.length == 1 && args[0].equals("siblings") ? 4 : 2)) throw new AssertionError("original class bridge count changed");
        Call[] calls = {
            (r, m) -> r.makeValue(m),
            (r, m) -> ((AbstractWidget) r).makeValue(m),
            (r, m) -> (Value) invoke(sdkMake, r, m),
            (r, m) -> (Value) invoke(broadMake, r, m),
            (r, m) -> r.defaultResult(),
            (r, m) -> (Value) invoke(typedDefault, r),
            (r, m) -> (Value) invoke(sdkDefault, r),
            (r, m) -> (Value) invoke(broadDefault, r)
        };
        Forwarder[] receivers = {new Forwarder(new Sink()), new Overriding()};
        Object[] markers = {null, "owned-marker", new Object()};
        for (Forwarder receiver : receivers) for (Object marker : markers)
            for (int route = 0; route < calls.length; route++) {
                int before = receiver.touches;
                Value result = calls[route].run(receiver, marker);
                identity(result, receiver.value);
                result.tag = marker;
                identity(receiver.value.tag, marker);
                state(receiver, before, marker, route < 4);
            }
        RuntimeException problem = new IllegalStateException("owned covariant class failure");
        for (Forwarder receiver : receivers) {
            receiver.failure = problem;
            for (int route = 0; route < calls.length; route++) {
                int before = receiver.touches;
                try { calls[route].run(receiver, problem); throw new AssertionError("bridge failure missing"); }
                catch (RuntimeException actual) { identity(actual, problem); failures++; }
                state(receiver, before, problem, route < 4);
            }
            receiver.failure = null;
        }
        for (Call call : calls) {
            try { call.run(null, problem); throw new AssertionError("null bridge receiver accepted"); }
            catch (NullPointerException expected) { failures++; }
        }
        System.out.println("plain-class-covariant-values=48,identities=" + identities + ",states=" + states
                + ",failures=" + failures + ",original-source-bridges=" + bridges);
        System.out.println("plain-class-covariance-and-full-metadata=retained,public-protected-and-subclass-dispatch=matched");
    }
}
