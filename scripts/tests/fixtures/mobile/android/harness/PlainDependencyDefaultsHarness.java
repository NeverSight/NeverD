import fixture.Worker;
import java.lang.reflect.*;
import java.util.function.Consumer;

public final class PlainDependencyDefaultsHarness {
    private static int values, identities, failures;
    private static void value(boolean expected) {
        if (!expected) throw new AssertionError("owned state mismatch");
        values++;
    }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("owned identity mismatch");
        identities++;
    }
    private static void failure(Runnable action, Class<?> type, Throwable sentinel) {
        try { action.run(); }
        catch (Throwable actual) {
            if (actual.getClass() != type || sentinel != null && actual != sentinel)
                throw new AssertionError("owned exception identity mismatch", actual);
            failures++;
            return;
        }
        throw new AssertionError("missing owned failure");
    }
    public static void main(String[] args) throws Exception {
        value(Worker.class.getMethod("andThen", Consumer.class).getDeclaringClass() == Consumer.class);
        value(Worker.class.getDeclaredMethod("accept", Object.class).getGenericParameterTypes()[0].getTypeName().equals("T"));
        value(Worker.class.getGenericInterfaces()[0].getTypeName().equals("java.util.function.Consumer<T>"));
        value(java.util.Arrays.stream(Worker.class.getDeclaredMethods()).noneMatch(method -> method.getName().equals("andThen")));
        for (boolean initial : new boolean[] {false, true})
            for (Object item : new Object[] {null, "owned value", new Object(), Integer.valueOf(-7)}) {
                Worker<Object, String> worker = new Worker<>(initial);
                Object[] received = new Object[1];
                int[] secondCalls = new int[1];
                Consumer<Object> after = result -> { received[0] = result; secondCalls[0]++; };
                Consumer<Object> chain = worker.andThen(after);
                chain.accept(item);
                value(worker.get());
                value(worker.calls == (initial ? 0 : 1));
                identity(worker.value, initial ? null : item);
                identity(received[0], item);
                value(secondCalls[0] == 1);
                chain.accept(null);
                value(worker.calls == (initial ? 0 : 1));
                identity(received[0], null);
                value(secondCalls[0] == 2);
                failure(() -> worker.andThen(null), NullPointerException.class, null);
                value(worker.calls == (initial ? 0 : 1));
                worker.set(false);
                RuntimeException sentinel = new IllegalStateException("owned first failure");
                worker.failure = sentinel;
                failure(() -> chain.accept(item), sentinel.getClass(), sentinel);
                value(worker.calls == (initial ? 1 : 2));
                value(worker.get());
                identity(worker.value, item);
                value(secondCalls[0] == 2);
                chain.accept(item);
                value(secondCalls[0] == 3);
                identity(received[0], item);
                worker.failure = null;
                worker.set(false);
                RuntimeException secondFailure = new IllegalArgumentException("owned second failure");
                Consumer<Object> failedAfter = result -> { received[0] = result; throw secondFailure; };
                failure(() -> worker.andThen(failedAfter).accept(item), secondFailure.getClass(), secondFailure);
                value(worker.calls == (initial ? 2 : 3));
                identity(worker.value, item);
                identity(received[0], item);
                value(worker.toString().equals("owned state"));
            }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("sdk-default-owner=retained,atomic-state-and-ordered-dispatch=matched");
    }
}
