import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public final class ClassFormalObjectReturnsHarness {
    private static int values, identities, states, failures;
    private interface Call { Object run() throws Throwable; }
    private static final class Sink implements Callback<String, byte[]> {
        public void changed(String key, byte[] value) {
            throw new AssertionError("echo called its identity-only callback");
        }
    }
    private static Object reflect(Method method, Object receiver, Object... args) throws Throwable {
        try { return method.invoke(receiver, args); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("class-formal source identity changed");
        identities++;
    }
    private static Call[] paths(Child child, Callback<String, byte[]> callback, boolean echo,
                                Method implementation, Method declaration) {
        Base<List<String>> parent = child;
        SourceView view = child;
        if (echo) {
            return new Call[] {
                () -> child.echo(callback), () -> parent.echo(callback), () -> view.echo(callback),
                () -> reflect(implementation, child, callback), () -> reflect(declaration, child, callback)
            };
        }
        return new Call[] {
            () -> child.getSource(), () -> parent.getSource(), () -> view.getSource(),
            () -> reflect(implementation, child), () -> reflect(declaration, child)
        };
    }
    private static void execute(Call call, Child child, List<String> source, Callback<String, byte[]> callback,
                                boolean echo, boolean absent, RuntimeException problem) {
        int gets = child.gets;
        int echoes = child.echoes;
        Callback<String, byte[]> previous = child.seen;
        child.events = 0;
        try {
            Object result = call.run();
            if (absent || problem != null) throw new AssertionError("source failure missing");
            same(result, source);
            values++;
        } catch (Throwable error) {
            Class<?> expected = absent ? NullPointerException.class : IllegalStateException.class;
            if ((!absent && problem == null) || error.getClass() != expected
                    || (problem != null && error != problem))
                throw new AssertionError("source exception type or identity changed", error);
            if (problem != null) identities++;
            failures++;
        }
        int delta = absent ? 0 : 1;
        if (child.gets != gets + (echo ? 0 : delta)
                || child.echoes != echoes + (echo ? delta : 0)
                || child.events != delta * (echo ? 2 : 1))
            throw new AssertionError("class-formal call count or failure order changed");
        same(child.seen, echo && !absent ? callback : previous);
        same(child.source, source);
        states++;
    }
    private static void callbackParameter(Method method) {
        Type parameter = method.getGenericParameterTypes()[0];
        if (!(parameter instanceof ParameterizedType)
                || ((ParameterizedType) parameter).getRawType() != Callback.class
                || !Arrays.equals(((ParameterizedType) parameter).getActualTypeArguments(),
                                  new Type[] {String.class, byte[].class}))
            throw new AssertionError("complete fixed callback parameter changed");
    }
    public static void main(String[] args) throws Exception {
        Method getter = Base.class.getDeclaredMethod("getSource");
        Method echo = Base.class.getDeclaredMethod("echo", Callback.class);
        Method viewGetter = SourceView.class.getDeclaredMethod("getSource");
        Method viewEcho = SourceView.class.getDeclaredMethod("echo", Callback.class);
        Type formal = Base.class.getTypeParameters()[0];
        if (!getter.getGenericReturnType().equals(formal) || !echo.getGenericReturnType().equals(formal)
                || !Base.class.getDeclaredField("source").getGenericType().equals(formal)
                || !Child.class.getGenericSuperclass().getTypeName()
                    .equals("fixture.Base<java.util.List<java.lang.String>>")
                || Base.class.getGenericInterfaces()[0] != SourceView.class)
            throw new AssertionError("original class-owned return formal or closed child scope changed");
        for (Method method : new Method[] {getter, echo, viewGetter, viewEcho}) {
            if (method.getReturnType() != Object.class || method.isBridge() || method.isSynthetic()
                    || method.getTypeParameters().length != 0 || method.getExceptionTypes().length != 0
                    || method.getGenericExceptionTypes().length != 0)
                throw new AssertionError("plain Object runtime entry acquired a bridge or method-owned scope");
        }
        callbackParameter(echo); callbackParameter(viewEcho);
        if (Child.class.getMethod("getSource").getDeclaringClass() != Base.class
                || Child.class.getMethod("echo", Callback.class).getDeclaringClass() != Base.class)
            throw new AssertionError("closed child acquired an invented implementation");
        for (Method method : Child.class.getDeclaredMethods()) {
            if (method.getName().equals("getSource") || method.getName().equals("echo"))
                throw new AssertionError("closed child acquired an invented return entry");
        }
        Child child = new Child();
        List<String> first = new ArrayList<String>(); first.add("first");
        List<String> second = new LinkedList<String>(); second.add("second");
        Callback<String, byte[]> sink = new Sink();
        for (List<String> source : Arrays.asList(first, second, null)) {
            child.source = source;
            Callback<String, byte[]> callback = source == second ? null : sink;
            for (Call call : paths(child, callback, false, getter, viewGetter))
                execute(call, child, source, callback, false, false, null);
            for (Call call : paths(child, callback, true, echo, viewEcho))
                execute(call, child, source, callback, true, false, null);
        }
        child.source = second;
        RuntimeException problem = new IllegalStateException("owned class-formal source failure");
        child.failure = problem;
        for (Call call : paths(child, sink, false, getter, viewGetter))
            execute(call, child, second, sink, false, false, problem);
        for (Call call : paths(child, sink, true, echo, viewEcho))
            execute(call, child, second, sink, true, false, problem);
        child.failure = null;
        for (Call call : paths(null, null, false, getter, viewGetter))
            execute(call, child, second, null, false, true, null);
        for (Call call : paths(null, null, true, echo, viewEcho))
            execute(call, child, second, null, true, true, null);
        System.out.println("values=" + values + ",identities=" + identities
                + ",states=" + states + ",failures=" + failures);
        System.out.println("class-owned-return-formals=retained,plain-object-runtime-entries=unchanged,closed-child-members=not-invented");
    }
}
