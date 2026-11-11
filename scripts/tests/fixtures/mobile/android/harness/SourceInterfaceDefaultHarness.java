import fixture.*;
import java.lang.reflect.*;

public final class SourceInterfaceDefaultHarness {
    private static int values, identities, failures;
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("value changed");
        values++;
    }
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
    private static final class Overriding extends Receiver<Object> {
        public Object echo(Object value) { Trace.events = Trace.events * 10 + 5; return value; }
        public int hidden() { Trace.events = Trace.events * 10 + 8; return 53; }
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    public static void main(String[] args) throws Exception {
        Receiver<Object> receiver = new Receiver<>();
        Child<Object> child = receiver;
        Root raw = receiver;
        Object token = new Object();
        Object[] cases = {token, "text", null, new int[]{1,2}, Integer.valueOf(31)};
        for (Object input : cases) {
            identity(receiver.echo(input), input); events(1);
            identity(child.echo(input), input); events(1);
            identity(raw.echo(input), input); events(1);
            identity(Root.identity(input), input); events(4);
        }
        value(receiver.scalar(), 17); events(23);
        Overriding overriding = new Overriding();
        identity(overriding.echo(token), token); events(5);
        Root<Object> rootView = overriding;
        identity(rootView.echo(token), token); events(5);
        value(rootView.scalar(), 17); events(23);
        value(overriding.hidden(), 53); events(8);
        RuntimeException problem = new IllegalStateException("owned default failure");
        Trace.firstFailure = problem;
        failure(() -> child.echo(token), IllegalStateException.class, problem, 1);
        identity(rootView.echo(token), token); events(5);
        Trace.firstFailure = null; Trace.secondFailure = problem;
        failure(() -> receiver.scalar(), IllegalStateException.class, problem, 2);
        failure(() -> rootView.scalar(), IllegalStateException.class, problem, 2);
        Trace.secondFailure = null; Trace.boxFailure = problem;
        failure(() -> Root.identity(token), IllegalStateException.class, problem, 4);
        Trace.boxFailure = null;
        Receiver<Object> absent = null;
        failure(() -> absent.echo(token), NullPointerException.class, null, 0);
        failure(() -> absent.scalar(), NullPointerException.class, null, 0);
        Method echo = Root.class.getMethod("echo", Object.class);
        Type formal = Root.class.getTypeParameters()[0];
        if (!echo.isDefault() || !echo.getGenericReturnType().equals(formal)
                || !echo.getGenericParameterTypes()[0].equals(formal))
            throw new AssertionError("default generic contract changed");
        Method hidden = Root.class.getDeclaredMethod("hidden");
        if (!Modifier.isPrivate(hidden.getModifiers()) || hidden.isDefault())
            throw new AssertionError("private interface method changed");
        if (!Modifier.isStatic(Root.class.getMethod("identity", Object.class).getModifiers())
                || Child.class.getDeclaredMethods().length != 0)
            throw new AssertionError("static or inherited method ownership changed");
        for (Method method : Receiver.class.getDeclaredMethods()) {
            if (!Modifier.isPrivate(method.getModifiers()) || !Modifier.isStatic(method.getModifiers())
                    || !method.getName().startsWith("__neverdThrow")
                    || method.getReturnType() != RuntimeException.class
                    || method.getParameterTypes().length != 1
                    || method.getParameterTypes()[0] != Throwable.class)
                throw new AssertionError("input implementation acquired an inherited method body");
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("default-static-private-dispatch=matched,generic-reflection=unchanged");
    }
}
