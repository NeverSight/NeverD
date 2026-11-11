import fixture.*;
import java.lang.reflect.*;

public final class SourceRawInterfaceParentHarness {
    private static int identities, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("identity changed");
        identities++;
    }
    private static void events(int expected) {
        if (Trace.events != expected) throw new AssertionError("effect order changed: " + Trace.events);
        Trace.events = 0;
    }
    private static void failure(Runnable call, Class<? extends Throwable> kind,
                                RuntimeException same, int effects) {
        try { call.run(); throw new AssertionError("failure missing"); }
        catch (Throwable e) {
            if (e.getClass() != kind || (same != null && e != same))
                throw new AssertionError("failure type or identity changed", e);
            if (same != null) identities++;
            failures++;
        }
        events(effects);
    }
    private static final class Overriding extends Child<Object> {
        public <M> M fold(M value) { Trace.events = Trace.events * 10 + 5; return value; }
    }
    public static void main(String[] args) throws Exception {
        Child<Object> child = new Child<>();
        Parent<Object> parent = child;
        Leaf leaf = child;
        Root root = child;
        View view = child;
        Object token = new Object();
        Box<Object> box = new Box<>(token);
        child.stored = box; child.value = token;
        Object[] cases = {token, "text", null, new int[]{1,2}, Integer.valueOf(31)};
        for (Object input : cases) {
            identity(child.fold(input), input); events(1);
            identity(leaf.fold(input), input); events(1);
            identity(root.fold(input), input); events(1);
        }
        identity(view.elements(), box); events(2);
        identity(parent.elements(), box); events(2);
        identity(child.make(), token); events(3);
        identity(parent.make(), token); events(3);
        identity(child.elements().value, token); events(2);
        child.stored = null;
        identity(view.elements(), null); events(2); child.stored = box;
        String text = child.fold("text"); identity(text, "text"); events(1);
        Root overridden = new Overriding();
        identity(overridden.fold(token), token); events(5);
        RuntimeException problem = new IllegalStateException("owned raw-interface failure");
        Trace.secondFailure = problem;
        failure(() -> root.fold(token), IllegalStateException.class, problem, 1);
        identity(overridden.fold(token), token); events(5);
        Trace.secondFailure = null; Trace.firstFailure = problem;
        failure(() -> view.elements(), IllegalStateException.class, problem, 2);
        Trace.firstFailure = null; Trace.targetFailure = problem;
        failure(() -> parent.make(), IllegalStateException.class, problem, 3);
        Trace.targetFailure = null;
        Child<Object> absent = null;
        failure(() -> absent.fold(token), NullPointerException.class, null, 0);
        failure(() -> absent.elements(), NullPointerException.class, null, 0);
        failure(() -> absent.make(), NullPointerException.class, null, 0);
        Method fold = Leaf.class.getMethod("fold", Object.class);
        Type formal = fold.getTypeParameters()[0];
        if (!fold.isDefault() || !fold.getGenericReturnType().equals(formal)
                || !fold.getGenericParameterTypes()[0].equals(formal))
            throw new AssertionError("method formal scope changed");
        Method elements = Parent.class.getMethod("elements");
        if (!Modifier.isFinal(elements.getModifiers())
                || !elements.getGenericReturnType().getTypeName().equals("fixture.Box<java.lang.Object>"))
            throw new AssertionError("full inherited reference contract changed");
        Method make = Child.class.getMethod("make");
        if (!make.getGenericReturnType().equals(Child.class.getTypeParameters()[0]))
            throw new AssertionError("child class formal scope changed");
        for (Method method : Child.class.getDeclaredMethods())
            if (method.getName().equals("fold") || method.getName().equals("elements"))
                throw new AssertionError("inherited method copied into child");
        System.out.println("identities=" + identities + ",failures=" + failures);
        System.out.println("raw-interface-and-source-parent=matched,full-generic-reflection=unchanged");
    }
}
