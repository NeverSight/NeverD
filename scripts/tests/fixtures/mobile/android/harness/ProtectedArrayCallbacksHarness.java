import fixture.Base;
import fixture.Owner;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

public final class ProtectedArrayCallbacksHarness {
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
        try {
            action.run();
        } catch (Throwable actual) {
            if (actual instanceof InvocationTargetException) actual = actual.getCause();
            if (actual.getClass() != type || sentinel != null && actual != sentinel)
                throw new AssertionError("owned exception identity mismatch", actual);
            failures++;
            return;
        }
        throw new AssertionError("missing owned failure");
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        Method run = Owner.Worker.class.getDeclaredMethod("run", Object[].class);
        Method cancel = Owner.Worker.class.getDeclaredMethod("cancel", Object.class);
        Method finish = Owner.Worker.class.getDeclaredMethod("finish", Object.class);
        for (Method bridge : new Method[] {run, cancel, finish}) {
            value(bridge.isBridge() && bridge.isSynthetic());
            value(Modifier.isProtected(bridge.getModifiers()) && !Modifier.isPublic(bridge.getModifiers()));
            value(!bridge.isVarArgs());
            bridge.setAccessible(true);
        }
        Method typed = Owner.Worker.class.getDeclaredMethod("run", String[].class);
        value(typed.isVarArgs() && Modifier.isProtected(typed.getModifiers()));
        value(!typed.isBridge() && !typed.isSynthetic());
        value(Owner.Worker.class.getGenericSuperclass().getTypeName().equals(
                "fixture.Base<java.lang.String, java.lang.Integer, java.lang.String>"));
        for (int index : new int[] {0, 1, 2}) {
            Owner owner = new Owner(index);
            Owner.Worker worker = owner.create();
            Base raw = worker;
            value(worker.cancelCalls == 1 && worker.finishCalls == 0 && owner.callbacks == 1);
            value(worker.trace == 5);
            String first = new String("owned first"), second = new String("owned second");
            String[] items = new String[] {first, second, null};
            worker.trace = 0;
            identity(raw.dispatch(items), items[index]);
            identity(worker.lastArray, items);
            identity(worker.lastResult, items[index]);
            value(worker.calls == 1 && worker.trace == 14);
            owner.index = (index + 1) % 3;
            worker.trace = 0;
            identity(run.invoke(worker, (Object) items), items[owner.index]);
            value(worker.calls == 2 && worker.trace == 4);
            identity(worker.lastArray, items);
            for (Object result : new Object[] {first, second, null}) {
                worker.trace = 0;
                raw.dispatchCancel(result);
                value(worker.trace == 25);
                identity(worker.lastResult, result);
                worker.trace = 0;
                raw.dispatchFinish(result);
                value(worker.trace == 36);
                identity(worker.lastResult, result);
            }
            value(worker.cancelCalls == 4 && worker.finishCalls == 3 && owner.callbacks == 7);
            worker.trace = 0;
            failure(() -> run.invoke(worker, (Object) new Object[] {first}), ClassCastException.class, null);
            value(worker.calls == 2 && worker.trace == 0);
            failure(() -> cancel.invoke(worker, new Object()), ClassCastException.class, null);
            failure(() -> finish.invoke(worker, Integer.valueOf(1)), ClassCastException.class, null);
            value(worker.cancelCalls == 4 && worker.finishCalls == 3 && owner.callbacks == 7);
            worker.trace = 0;
            failure(() -> raw.dispatch(null), NullPointerException.class, null);
            identity(worker.lastArray, null);
            value(worker.calls == 3 && worker.trace == 14);
            worker.trace = 0;
            failure(() -> raw.dispatch(new String[0]), ArrayIndexOutOfBoundsException.class, null);
            value(worker.calls == 4 && worker.trace == 14);
            RuntimeException sentinel = new RuntimeException("owned target failure");
            worker.failure = sentinel;
            worker.trace = 0;
            failure(() -> raw.dispatch(items), sentinel.getClass(), sentinel);
            value(worker.calls == 5 && worker.trace == 14);
            identity(worker.lastArray, items);
            worker.trace = 0;
            failure(() -> raw.dispatchCancel(first), sentinel.getClass(), sentinel);
            value(worker.trace == 25 && worker.cancelCalls == 5 && owner.callbacks == 8);
            identity(worker.lastResult, first);
            worker.trace = 0;
            failure(() -> raw.dispatchFinish(second), sentinel.getClass(), sentinel);
            value(worker.trace == 36 && worker.finishCalls == 4 && owner.callbacks == 9);
            identity(worker.lastResult, second);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("protected-array-and-void-dispatch=matched,mutable-capture-and-constructor-order=matched");
    }
}
