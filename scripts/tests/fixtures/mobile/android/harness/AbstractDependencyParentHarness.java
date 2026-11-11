import fixture.*;
import java.lang.reflect.*;

public final class AbstractDependencyParentHarness {
    private static int identities, states, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("abstract SDK identity changed");
        identities++;
    }
    private static void state(Root receiver, int expected, Object marker) {
        if (receiver.stamp != 7 || receiver.touches != expected)
            throw new AssertionError("abstract SDK provider effects changed");
        identity(receiver.seen, marker);
        states++;
    }
    private static Object invoke(Method method, Object receiver, Object... arguments) throws Throwable {
        try { return method.invoke(receiver, arguments); }
        catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    private static final class Overriding extends AbstractWidget {
        public Object item() { return seen; }
        protected void touch(Object marker) {
            touches += 4;
            seen = marker;
            if (failure != null) throw failure;
        }
    }
    private static void formal(Method method, String name) {
        TypeVariable<Method>[] variables = method.getTypeParameters();
        if (variables.length != 1 || !variables[0].getName().equals(name)
                || !variables[0].getGenericDeclaration().equals(method)
                || variables[0].getBounds()[0] != Object.class
                || !method.getGenericParameterTypes()[0].equals(variables[0])
                || !method.getGenericReturnType().equals(variables[0]))
            throw new AssertionError("complete original method-owned formal changed");
    }
    public static void main(String[] args) throws Throwable {
        OriginalRawParametersHarness.main(args);
        if (Forwarder.class.getSuperclass() != AbstractWidget.class
                || AbstractWidget.class.getSuperclass() != Root.class
                || !Modifier.isAbstract(AbstractWidget.class.getModifiers())
                || AbstractWidget.class.getTypeParameters().length != 0
                || Forwarder.class.getTypeParameters().length != 0)
            throw new AssertionError("independent abstract SDK class edges changed");
        Method touch = AbstractWidget.class.getDeclaredMethod("touch", Object.class);
        Method sourceTouch = Forwarder.class.getDeclaredMethod("touch", Object.class);
        Method item = AbstractWidget.class.getDeclaredMethod("item");
        if (!Modifier.isAbstract(touch.getModifiers()) || !Modifier.isProtected(touch.getModifiers())
                || !Modifier.isAbstract(item.getModifiers())
                || !Modifier.isProtected(sourceTouch.getModifiers())
                || Modifier.isAbstract(sourceTouch.getModifiers())
                || sourceTouch.getGenericParameterTypes()[0] != Object.class
                || Forwarder.class.getMethod("item").getDeclaringClass() != Forwarder.class)
            throw new AssertionError("original source provider or protected SDK requirement changed");
        touch.setAccessible(true);
        Method sdkIdentity = AbstractWidget.class.getDeclaredMethod("identity", Object.class);
        formal(sdkIdentity, "M");
        formal(Forwarder.class.getDeclaredMethod("identity", Object.class), "N");
        for (Method method : Forwarder.class.getDeclaredMethods())
            if (method.isBridge()) throw new AssertionError("abstract SDK proof invented a source bridge");
        Forwarder receiver = new Forwarder(new Sink());
        Object marker = new Object();
        receiver.ping(marker);
        state(receiver, 1, marker);
        invoke(touch, receiver, marker);
        state(receiver, 2, marker);
        identity(receiver.item(), marker);
        identity(invoke(item, receiver), marker);
        RuntimeException problem = new IllegalStateException("owned abstract SDK failure");
        receiver.failure = problem;
        for (int route = 0; route < 2; route++) {
            try {
                if (route == 0) receiver.ping(marker); else invoke(touch, receiver, marker);
                throw new AssertionError("original provider failure missing");
            } catch (RuntimeException actual) { identity(actual, problem); failures++; }
            state(receiver, 3 + route, marker);
        }
        receiver.failure = null;
        receiver.ping(null);
        state(receiver, 5, null);
        Object value = new Object();
        identity(receiver.identity(value), value);
        identity(invoke(sdkIdentity, receiver, value), value);
        state(receiver, 7, null);
        Overriding child = new Overriding();
        invoke(touch, child, marker);
        state(child, 4, marker);
        identity(invoke(item, child), marker);
        identity(invoke(sdkIdentity, child, marker), marker);
        state(child, 5, marker);
        child.failure = problem;
        try { invoke(touch, child, (Object) null); throw new AssertionError("virtual provider failure missing"); }
        catch (RuntimeException actual) { identity(actual, problem); failures++; }
        state(child, 9, null);
        System.out.println("abstract-sdk-identities=" + identities + ",abstract-sdk-states=" + states
                + ",abstract-sdk-failures=" + failures + ",source-bridges=0");
        System.out.println("abstract-sdk-requirements-and-original-provider-ownership=retained,method-formals-and-virtual-dispatch=matched");
    }
}
