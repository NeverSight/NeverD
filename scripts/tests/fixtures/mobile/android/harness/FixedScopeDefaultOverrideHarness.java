import fixture.*;
import java.lang.reflect.Method;

public final class FixedScopeDefaultOverrideHarness {
    private static int identities, failures, values;
    private interface Operation { void run(); }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("identity " + identities);
        ++identities;
    }
    private static void value(boolean condition) {
        if (!condition) throw new AssertionError("value " + values);
        ++values;
    }
    private static void fail(Class<? extends Throwable> type, Throwable sentinel, Operation operation) {
        try { operation.run(); }
        catch (Throwable actual) {
            if (actual.getClass() != type || (sentinel != null && actual != sentinel))
                throw new AssertionError("failure " + failures, actual);
            ++failures;
            return;
        }
        throw new AssertionError("missing failure " + failures);
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        if (!Receiver.class.getGenericSuperclass().getTypeName().equals("fixture.Middle<java.lang.String>")
                || !Receiver.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Api<java.lang.String>")
                || !Middle.class.getGenericSuperclass().getTypeName().equals("fixture.Root<M>")
                || !Root.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Api<R>"))
            throw new AssertionError("complete fixed scope reflection");
        int bridges = 0;
        for (Method method : Receiver.class.getDeclaredMethods()) if (method.isBridge()) ++bridges;
        if (bridges != 1 || Receiver.class.getDeclaredMethod("echo", String.class).getTypeParameters().length != 0)
            throw new AssertionError("original bridge inventory");
        Receiver receiver = new Receiver();
        Api<String> api = receiver;
        Api raw = receiver;
        Middle<String> middle = receiver;
        Root<String> root = receiver;
        for (String token : new String[]{null, new String("first"), new String("last")}) {
            int before = receiver.calls;
            same(receiver.echo(token), token);
            same(api.echo(token), token);
            same(raw.echo(token), token);
            same(middle.echo(token), token);
            same(root.echo(token), token);
            same(receiver.last, token);
            value(receiver.calls == before + 5 && Trace.calls == 0);
            same(receiver.echo((Integer) null), "overload");
        }
        value(receiver.overloads == 3);
        int before = receiver.calls;
        fail(ClassCastException.class, null, () -> raw.echo(Integer.valueOf(4)));
        value(receiver.calls == before && Trace.calls == 0);
        RuntimeException sentinel = new RuntimeException("owned failure");
        receiver.failure = sentinel;
        for (String token : new String[]{null, new String("exception")}) {
            before = receiver.calls;
            fail(RuntimeException.class, sentinel, () -> receiver.echo(token));
            fail(RuntimeException.class, sentinel, () -> api.echo(token));
            fail(RuntimeException.class, sentinel, () -> raw.echo(token));
            fail(RuntimeException.class, sentinel, () -> middle.echo(token));
            fail(RuntimeException.class, sentinel, () -> root.echo(token));
            same(receiver.last, token);
            value(receiver.calls == before + 5 && Trace.calls == 0);
        }
        Root<Object> unchanged = new Root<Object>();
        Object token = new Object();
        same(unchanged.echo(token), token);
        same(Trace.last, token);
        value(Trace.calls == 1);
        Trace.failure = sentinel;
        fail(RuntimeException.class, sentinel, () -> unchanged.echo(null));
        same(Trace.last, null);
        value(Trace.calls == 2 && receiver.calls == 25);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("fixed-scope=retained,ancestor-default-effects=unchanged,override-effects=ordered");
    }
}
