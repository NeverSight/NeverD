import fixture.*;
import java.lang.reflect.*;

public final class FixedDependencyClassPrefixHarness {
    private static int identities, states, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("SDK alias identity changed");
        identities++;
    }
    private static void state(Root receiver, int expected, Object marker) {
        if (receiver.stamp != 7 || receiver.touches != expected)
            throw new AssertionError("SDK construction or touch effects changed");
        identity(receiver.seen, marker);
        states++;
    }
    private static Object invoke(Method method, Object receiver, Object... arguments) throws Throwable {
        try { return method.invoke(receiver, arguments); }
        catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    private static final class Overriding extends Widget {
        public String item() { return "owned-virtual-item"; }
        protected void touch(Object marker) {
            touches += 3;
            super.touch(marker);
        }
    }
    public static void main(String[] args) throws Throwable {
        OriginalRawParametersHarness.main(args);
        if (Forwarder.class.getSuperclass() != Widget.class
                || Widget.class.getSuperclass() != Fixed.class
                || Base.class.getSuperclass() != Root.class
                || Forwarder.class.getTypeParameters().length != 0
                || Widget.class.getTypeParameters().length != 0
                || Fixed.class.getTypeParameters().length != 0)
            throw new AssertionError("original independent SDK class edges changed");
        ParameterizedType fixed = (ParameterizedType) Fixed.class.getGenericSuperclass();
        if (fixed.getRawType() != Base.class || fixed.getActualTypeArguments()[0] != String.class)
            throw new AssertionError("original fixed SDK class argument changed");
        Method genericItem = Base.class.getDeclaredMethod("item");
        TypeVariable<?> formal = Base.class.getTypeParameters()[0];
        if (!genericItem.getGenericReturnType().equals(formal)
                || formal.getGenericDeclaration() != Base.class
                || Forwarder.class.getMethod("item").getDeclaringClass() != Widget.class
                || Forwarder.class.getMethod("set", String.class).getDeclaringClass() != Widget.class)
            throw new AssertionError("SDK methods or their class formal were copied into source");
        int bridges = 0;
        for (Method method : Widget.class.getDeclaredMethods()) if (method.isBridge()) bridges++;
        for (Method method : Forwarder.class.getDeclaredMethods())
            if (method.isBridge()) throw new AssertionError("source acquired a generic SDK bridge");
        if (bridges != 2) throw new AssertionError("original SDK bridges changed");
        Method touch = Root.class.getDeclaredMethod("touch", Object.class);
        touch.setAccessible(true);
        Method sourceTouch = Forwarder.class.getDeclaredMethod("touch", Object.class);
        if (!Modifier.isProtected(sourceTouch.getModifiers())
                || sourceTouch.getGenericParameterTypes()[0] != Object.class)
            throw new AssertionError("original protected source override changed");
        Forwarder receiver = new Forwarder(new Sink());
        Base<String> alias = receiver;
        for (String value : new String[] {null, "owned-value", new String("owned-value")}) {
            alias.set(value);
            identity(receiver.item(), value);
            identity(invoke(genericItem, receiver), value);
        }
        Object marker = new Object();
        int before = receiver.touches;
        receiver.ping(marker);
        state(receiver, before + 1, marker);
        invoke(touch, receiver, marker);
        state(receiver, before + 2, marker);
        RuntimeException problem = new IllegalStateException("owned SDK touch failure");
        receiver.failure = problem;
        for (int route = 0; route < 2; route++) {
            try {
                if (route == 0) receiver.ping(marker); else invoke(touch, receiver, marker);
                throw new AssertionError("SDK failure missing");
            } catch (RuntimeException actual) { identity(actual, problem); failures++; }
            state(receiver, before + 3 + route, marker);
        }
        receiver.failure = null;
        receiver.ping(null);
        state(receiver, before + 5, null);
        Overriding child = new Overriding();
        identity(invoke(genericItem, child), "owned-virtual-item");
        invoke(touch, child, marker);
        state(child, 4, marker);
        child.failure = problem;
        try { invoke(touch, child, (Object) null); throw new AssertionError("virtual SDK failure missing"); }
        catch (RuntimeException actual) { identity(actual, problem); failures++; }
        state(child, 8, null);
        System.out.println("sdk-identities=" + identities + ",sdk-states=" + states
                + ",sdk-failures=" + failures + ",original-sdk-bridges=2,source-bridges=0");
        System.out.println("fixed-sdk-class-edges=retained,protected-source-and-sdk-virtual-dispatch=matched");
    }
}
