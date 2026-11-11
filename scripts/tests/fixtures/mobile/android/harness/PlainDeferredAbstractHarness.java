import fixture.*;
import java.lang.reflect.*;

public final class PlainDeferredAbstractHarness {
    private static int values, identities, states, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("deferred abstract identity changed");
        identities++;
    }
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("deferred abstract value changed");
        values++;
    }
    private static void state(int actual, int expected) {
        if (actual != expected) throw new AssertionError("deferred abstract effect changed");
        states++;
    }
    private static Object invoke(Method method, Object receiver, Object... arguments) throws Exception {
        try { return method.invoke(receiver, arguments); }
        catch (InvocationTargetException error) {
            Throwable cause = error.getCause();
            if (cause instanceof RuntimeException) throw (RuntimeException) cause;
            throw error;
        }
    }
    private static void formal(Class<?> owner, String name, boolean abstractMethod) throws Exception {
        Method method = owner.getDeclaredMethod("identity", Object.class);
        TypeVariable<Method>[] formals = method.getTypeParameters();
        if (formals.length != 1 || !formals[0].getName().equals(name)
                || formals[0].getGenericDeclaration() != method
                || method.getGenericParameterTypes()[0] != formals[0]
                || method.getGenericReturnType() != formals[0]
                || Modifier.isAbstract(method.getModifiers()) != abstractMethod)
            throw new AssertionError("deferred abstract method scope changed");
    }
    private static final class Independent extends Redeclared {
        Independent(Sink sink) { super(sink); }
        protected void touch(Object marker) {
            touches += 7; seen = marker;
            if (failure != null) throw failure;
        }
        public Object item() { return seen; }
        public <S> S identity(S marker) {
            touches += 5;
            if (failure != null) throw failure;
            return marker;
        }
    }
    private interface Call { Object run(Redeclared receiver, Object marker) throws Exception; }
    @SuppressWarnings("unchecked")
    public static void main(String[] args) throws Throwable {
        AbstractDependencyParentHarness.main(args);
        for (Class<?> owner : new Class<?>[] {Deferred.class, Redeclared.class}) {
            if (!Modifier.isAbstract(owner.getModifiers()) || owner.getTypeParameters().length != 0)
                throw new AssertionError("deferred abstract class shape changed");
            for (Method method : owner.getDeclaredMethods())
                if (method.isBridge()) throw new AssertionError("deferred abstract bridge invented");
        }
        if (Deferred.class.getSuperclass() != AbstractWidget.class
                || Redeclared.class.getSuperclass() != Deferred.class
                || Deferred.class.getMethod("item").getDeclaringClass() != AbstractWidget.class
                || Redeclared.class.getMethod("item").getDeclaringClass() != Redeclared.class)
            throw new AssertionError("nearest original abstract owner changed");
        try { Deferred.class.getDeclaredMethod("touch", Object.class);
            throw new AssertionError("inherited abstract method copied");
        } catch (NoSuchMethodException expected) {}
        formal(AbstractWidget.class, "M", false);
        formal(Deferred.class, "N", true);
        formal(Redeclared.class, "Q", true);
        formal(Resolved.class, "R", false);
        Method sdkTouch = AbstractWidget.class.getDeclaredMethod("touch", Object.class);
        Method sourceTouch = Redeclared.class.getDeclaredMethod("touch", Object.class);
        sdkTouch.setAccessible(true); sourceTouch.setAccessible(true);
        if (!Modifier.isAbstract(sdkTouch.getModifiers()) || !Modifier.isProtected(sdkTouch.getModifiers())
                || !Modifier.isAbstract(sourceTouch.getModifiers()) || !Modifier.isProtected(sourceTouch.getModifiers()))
            throw new AssertionError("protected abstract declaration changed");
        Method ping = Deferred.class.getMethod("ping", Object.class);
        Method sdkItem = AbstractWidget.class.getDeclaredMethod("item");
        Method sourceItem = Redeclared.class.getDeclaredMethod("item");
        Method sdkIdentity = AbstractWidget.class.getDeclaredMethod("identity", Object.class);
        Method baseIdentity = Deferred.class.getDeclaredMethod("identity", Object.class);
        Method sourceIdentity = Redeclared.class.getDeclaredMethod("identity", Object.class);
        Call[] touches = {
            (r, m) -> { r.ping(m); return null; },
            (r, m) -> invoke(ping, r, m),
            (r, m) -> invoke(sdkTouch, r, m),
            (r, m) -> invoke(sourceTouch, r, m)
        };
        Call[] items = {(r, m) -> r.pull(), (r, m) -> invoke(sdkItem, r), (r, m) -> invoke(sourceItem, r)};
        Call[] generic = {
            (r, m) -> r.identity(m), (r, m) -> invoke(sdkIdentity, r, m),
            (r, m) -> invoke(baseIdentity, r, m), (r, m) -> invoke(sourceIdentity, r, m)
        };
        Redeclared[] receivers = {new Resolved(new Sink()), new Independent(new Sink())};
        RawBox<Object> box = new RawBox<>(new Object());
        Object[] markers = {null, new Object(), box};
        for (Redeclared receiver : receivers) {
            int touchStep = receiver instanceof Resolved ? 3 : 7;
            int identityStep = receiver instanceof Resolved ? 2 : 5;
            value(receiver.stamp, 7);
            for (Object marker : markers) {
                for (Call call : touches) {
                    int before = receiver.touches;
                    call.run(receiver, marker);
                    identity(receiver.seen, marker); state(receiver.touches, before + touchStep);
                }
                for (Call call : items) {
                    Object result = call.run(receiver, marker); identity(result, marker);
                    if (result instanceof RawBox) {
                        Object alias = new Object(); ((RawBox<Object>) result).value = alias;
                        identity(box.value, alias);
                    }
                }
                for (Call call : generic) {
                    int before = receiver.touches;
                    identity(call.run(receiver, marker), marker); state(receiver.touches, before + identityStep);
                }
            }
            Method accept = Deferred.class.getDeclaredMethod("accept", RawBox.class, Object.class);
            if (accept.getGenericParameterTypes()[0] != RawBox.class || accept.isBridge())
                throw new AssertionError("original abstract class raw callback changed");
            for (RawBox<?> input : new RawBox<?>[] {null, box})
                for (Object tag : new Object[] {null, new Object()})
                    for (boolean reflected : new boolean[] {false, true}) {
                        int before = receiver.sink.calls; Trace.events = 0;
                        if (reflected) invoke(accept, receiver, input, tag); else ((Api) receiver).accept(input, tag);
                        identity(receiver.sink.seen, input); identity(receiver.sink.tag, tag);
                        state(receiver.sink.calls, before + 1); value(Trace.events, 31);
                    }
            RuntimeException problem = new IllegalStateException("owned deferred failure");
            receiver.failure = problem;
            for (Call call : touches) {
                int before = receiver.touches;
                try { call.run(receiver, box); throw new AssertionError("abstract touch failure lost"); }
                catch (RuntimeException actual) { identity(actual, problem); failures++; }
                identity(receiver.seen, box); state(receiver.touches, before + touchStep);
            }
            for (Call call : generic) {
                int before = receiver.touches;
                try { call.run(receiver, box); throw new AssertionError("abstract identity failure lost"); }
                catch (RuntimeException actual) { identity(actual, problem); failures++; }
                state(receiver.touches, before + identityStep);
            }
        }
        System.out.println("plain-deferred-abstract-values=" + values + ",identities=" + identities
                + ",states=" + states + ",failures=" + failures + ",bridges=0");
        System.out.println("nearest-abstract-declarations-and-full-method-scopes=retained,independent-concrete-subclass-dispatch=matched");
    }
}
