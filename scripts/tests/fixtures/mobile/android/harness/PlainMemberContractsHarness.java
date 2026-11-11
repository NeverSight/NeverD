import fixture.Worker;
import java.lang.reflect.*;
import java.util.function.Consumer;

public final class PlainMemberContractsHarness {
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
        value(fixture.PlainState.class.getTypeParameters().length == 0);
        value(fixture.PlainState.class.getMethod("metadata").getGenericReturnType().getTypeName().equals("java.util.List<java.lang.String>"));
        value(fixture.View.class.getMethod("metadata").getGenericReturnType().getTypeName().equals("java.util.List<java.lang.String>"));
        Method echo = fixture.PlainState.class.getMethod("echo", Object.class);
        value(echo.getTypeParameters().length == 1);
        identity(echo.getGenericParameterTypes()[0], echo.getTypeParameters()[0]);
        identity(echo.getGenericReturnType(), echo.getTypeParameters()[0]);
        Constructor<?> ctor = fixture.PlainState.class.getConstructor(boolean.class, java.util.List.class, Consumer.class);
        value(ctor.getGenericParameterTypes()[1].getTypeName().equals("java.util.List<java.lang.String>"));
        value(ctor.getGenericParameterTypes()[2].getTypeName().equals("java.util.function.Consumer<java.lang.String>"));
        java.util.List<String> parentEntries = new java.util.ArrayList<>();
        String[] parentReceived = new String[1];
        int[] parentCalls = new int[1];
        Consumer<String> parentCallback = input -> { parentReceived[0] = input; parentCalls[0]++; };
        fixture.PlainState parent = new fixture.PlainState(true, parentEntries, parentCallback);
        value(parent.get());
        value(parentReceived[0].equals("owned construction"));
        identity(parent.metadata(), parentEntries);
        identity(parent.echo(parentEntries), parentEntries);
        String parentSent = new String("owned parent callback");
        parent.notify(parentSent);
        identity(parentReceived[0], parentSent);
        value(parentCalls[0] == 2);
        for (boolean initial : new boolean[] {false, true})
            for (Object item : new Object[] {null, "owned value", new Object(), Integer.valueOf(-7)}) {
                java.util.List<String> entries = new java.util.ArrayList<>();
                entries.add("owned entry");
                String[] callbackValue = new String[1];
                int[] callbackCalls = new int[1];
                Consumer<String> callback = input -> { callbackValue[0] = input; callbackCalls[0]++; };
                Worker<Object, String> worker = new Worker<>(initial, entries, callback);
                value(callbackCalls[0] == 1);
                value(callbackValue[0].equals("owned construction"));
                identity(worker.metadata(), entries);
                identity(((fixture.View) worker).metadata(), entries);
                identity(worker.echo(item), item);
                identity(worker.echo(null), null);
                String sent = new String("owned callback");
                worker.notify(sent);
                identity(callbackValue[0], sent);
                value(callbackCalls[0] == 2);
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
        System.out.println("sdk-default-owner=retained,complete-parent-member-contracts-and-ordered-effects=matched");
    }
}
