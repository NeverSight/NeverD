import fixture.Echo;
import fixture.Worker;
import java.lang.reflect.*;
import java.util.concurrent.Callable;

public final class InheritedExceptionBridgesHarness {
    private static int values, identities, failures;
    private static void value(boolean expected) {
        if (!expected) throw new AssertionError("owned state mismatch");
        values++;
    }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("owned identity mismatch");
        identities++;
    }
    private interface Action { void run() throws Throwable; }
    private static void failure(Action action, Class<?> type, Throwable sentinel) throws Throwable {
        try { action.run(); }
        catch (Throwable actual) {
            if (actual instanceof InvocationTargetException) actual = actual.getCause();
            if (actual.getClass() != type || sentinel != null && sentinel != actual)
                throw new AssertionError("owned exception identity mismatch", actual);
            failures++;
            return;
        }
        throw new AssertionError("missing owned failure");
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        Method call = null;
        Method echo = Worker.class.getDeclaredMethod("echo", Object.class);
        for (Method method : Worker.class.getDeclaredMethods())
            if (method.getName().equals("call") && method.getReturnType() == Object.class) call = method;
        if (call == null) throw new AssertionError("missing owned erased call");
        final Method erasedCall = call;
        for (Method bridge : new Method[] {call, echo}) {
            value(bridge.isBridge() && bridge.isSynthetic() && Modifier.isPublic(bridge.getModifiers()));
        }
        value(java.util.Arrays.equals(call.getExceptionTypes(), new Class<?>[] {Exception.class}));
        value(java.util.Arrays.equals(echo.getExceptionTypes(),
                new Class<?>[] {java.io.IOException.class, ReflectiveOperationException.class}));
        value(Worker.class.getDeclaredMethod("call").getReturnType() == String.class);
        value(Worker.class.getDeclaredMethod("echo", String.class).getExceptionTypes().length == 0);
        for (Method method : Worker.class.getDeclaredMethods())
            if (method.getName().equals("call") && method.getReturnType() == String.class)
                value(method.getExceptionTypes().length == 0);
        value(Worker.class.getGenericInterfaces()[0].getTypeName().equals("java.util.concurrent.Callable<java.lang.String>"));
        value(Worker.class.getGenericInterfaces()[1].getTypeName().equals("fixture.Echo<java.lang.String>"));
        value(java.util.Arrays.equals(Echo.class.getDeclaredMethod("echo", Object.class).getExceptionTypes(),
                new Class<?>[] {java.io.IOException.class, ReflectiveOperationException.class}));
        value(Worker.class.getDeclaredConstructor().getExceptionTypes().length == 0);
        for (String text : new String[] {null, "", new String("owned result"), new String("owned result")}) {
            Worker worker = new Worker();
            Callable raw = worker;
            Echo rawEcho = worker;
            worker.value = text;
            identity(worker.call(), text);
            identity(raw.call(), text);
            identity(erasedCall.invoke(worker), text);
            value(worker.calls == 3);
            identity(worker.echo(text), text);
            identity(rawEcho.echo(text), text);
            identity(echo.invoke(worker, text), text);
            identity(worker.last, text);
            value(worker.echoes == 3);
            failure(() -> rawEcho.echo(new Object()), ClassCastException.class, null);
            value(worker.echoes == 3);
            identity(worker.last, text);
            RuntimeException sentinel = new IllegalArgumentException("owned failure");
            worker.failure = sentinel;
            failure(() -> worker.call(), sentinel.getClass(), sentinel);
            failure(() -> raw.call(), sentinel.getClass(), sentinel);
            failure(() -> erasedCall.invoke(worker), sentinel.getClass(), sentinel);
            value(worker.calls == 6);
            failure(() -> worker.echo(null), sentinel.getClass(), sentinel);
            failure(() -> rawEcho.echo(null), sentinel.getClass(), sentinel);
            failure(() -> echo.invoke(worker, new Object[] {null}), sentinel.getClass(), sentinel);
            value(worker.echoes == 6);
            identity(worker.last, null);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("original-bridge-exception-order=matched,typed-no-throws-and-dispatch=matched");
    }
}
