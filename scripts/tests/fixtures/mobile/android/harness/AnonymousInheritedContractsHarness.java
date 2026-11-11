import fixture.*;
import java.lang.reflect.*;
import java.util.concurrent.Executor;

public final class AnonymousInheritedContractsHarness {
    private static int values, identities, failures;
    private static void value(boolean ok) {
        ++values;
        if (!ok) throw new AssertionError("value " + values);
    }
    private static void identity(Object actual, Object expected) {
        ++identities;
        if (actual != expected) throw new AssertionError("identity " + identities);
    }
    private static void failure(Runnable action, Class<?> type, RuntimeException sentinel) {
        try { action.run(); throw new AssertionError("missing original failure"); }
        catch (RuntimeException actual) {
            ++failures;
            identity(actual.getClass(), type);
            if (sentinel != null) identity(actual, sentinel);
        }
    }
    private static Type argument(Type type) {
        return ((ParameterizedType) type).getActualTypeArguments()[0];
    }
    public static void main(String[] args) throws Exception {
        boolean recovered = args[0].equals("recovered");
        Class<?> member = null;
        for (Object token : new Object[] {new Object(), "owned", Integer.valueOf(73), null}) {
            Owner<Object> owner = new Owner<Object>(token);
            Root<Object> root = owner.view();
            Reader<Object> reader = root;
            member = root.getClass();
            identity(owner.view(), root);
            identity(root.initial, null);
            value(owner.calls == 1);
            identity(root.read(), token);
            identity(owner.read(), token);
            value(owner.calls == 3);
            Object next = new Object();
            owner.value = next;
            identity(root.read(), next);
            identity(owner.read(), next);
            value(owner.calls == 5);
            Method echo = member.getDeclaredMethod("echo", Object.class);
            echo.setAccessible(true);
            identity(echo.invoke(root, next), next);
            value(owner.calls == 5);
            value(echo.getTypeParameters().length == 1 &&
                    ((TypeVariable<?>) echo.getGenericReturnType()).getGenericDeclaration().equals(echo));
            value(root.ready() && reader.ready());
            int[] trace = {0};
            Runnable task = () -> trace[0] = trace[0] * 10 + 2;
            Executor executor = action -> { trace[0] = trace[0] * 10 + 1; action.run(); };
            reader.listen(task, executor);
            value(trace[0] == 12);
            value(root.listenerCalls == 1);
            identity(root.lastTask, task);
            identity(root.lastExecutor, executor);
            RuntimeException sentinel = new IllegalStateException("owned inherited failure");
            owner.failure = sentinel;
            failure(() -> root.read(), sentinel.getClass(), sentinel);
            value(owner.calls == 6);
            failure(() -> owner.read(), sentinel.getClass(), sentinel);
            value(owner.calls == 7);
            owner.failure = null;
            Runnable badTask = () -> { trace[0] = trace[0] * 10 + 2; throw sentinel; };
            trace[0] = 0;
            failure(() -> reader.listen(badTask, executor), sentinel.getClass(), sentinel);
            value(trace[0] == 12);
            value(root.listenerCalls == 2);
            identity(root.lastTask, badTask);
            Executor badExecutor = action -> { trace[0] = 1; throw sentinel; };
            trace[0] = 0;
            failure(() -> reader.listen(task, badExecutor), sentinel.getClass(), sentinel);
            value(trace[0] == 1);
            value(root.listenerCalls == 3);
            identity(root.lastExecutor, badExecutor);
            trace[0] = 0;
            failure(() -> reader.listen(task, null), NullPointerException.class, null);
            value(trace[0] == 0);
            value(root.listenerCalls == 4);
            identity(root.lastTask, task);
            identity(root.lastExecutor, null);
            trace[0] = 0;
            failure(() -> reader.listen(null, executor), NullPointerException.class, null);
            value(trace[0] == 1);
            value(root.listenerCalls == 5);
            identity(root.lastTask, null);
            trace[0] = 0;
            failure(() -> ((Reader<?>) null).listen(task, executor), NullPointerException.class, null);
            value(trace[0] == 0);
            value(root.listenerCalls == 5);
        }
        Constructor<?> constructor = member.getDeclaredConstructor(Owner.class);
        constructor.setAccessible(true);
        try { constructor.newInstance(new Object[] {null}); throw new AssertionError("missing null capture failure"); }
        catch (InvocationTargetException error) {
            ++failures;
            identity(error.getCause().getClass(), NullPointerException.class);
        }
        for (String name : new String[] {"read", "listen", "ready"}) {
            for (Method method : member.getDeclaredMethods())
                if (method.getName().equals(name)) throw new AssertionError("inherited body duplicated");
            value(true);
        }
        identity(member.getMethod("read").getDeclaringClass(), Root.class);
        identity(member.getMethod("listen", Runnable.class, Executor.class).getDeclaringClass(), Root.class);
        identity(member.getMethod("ready").getDeclaringClass(), Reader.class);
        Type inherited = argument(member.getGenericSuperclass());
        Type pending = member.getDeclaredMethod("pending").getGenericReturnType();
        value(recovered ? inherited == Object.class :
                inherited instanceof TypeVariable && ((TypeVariable<?>) inherited).getGenericDeclaration() == Owner.class);
        value(recovered ? pending == Object.class :
                pending instanceof TypeVariable && ((TypeVariable<?>) pending).getGenericDeclaration() == Owner.class);
        value(recovered ? member.isMemberClass() && Modifier.isStatic(member.getModifiers()) : member.isAnonymousClass());
        identity(member.getEnclosingClass(), Owner.class);
        Type ownerResult = Owner.class.getDeclaredMethod("read").getGenericReturnType();
        value(ownerResult instanceof TypeVariable && ((TypeVariable<?>) ownerResult).getGenericDeclaration() == Owner.class);
        value(argument(Owner.class.getDeclaredMethod("view").getGenericReturnType()).equals(ownerResult));
        Type rootResult = Root.class.getDeclaredMethod("read").getGenericReturnType();
        value(rootResult instanceof TypeVariable && ((TypeVariable<?>) rootResult).getGenericDeclaration() == Root.class);
        value(!Modifier.isAbstract(Reader.class.getDeclaredMethod("ready").getModifiers()));
        value(member.getDeclaredField("this$0").isSynthetic() != recovered);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("inherited-bodies=retained,default-dispatch=matched,mutable-capture-and-constructor-order=matched");
    }
}
