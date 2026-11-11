import fixture.*;
import java.lang.reflect.*;

public final class InheritedIntegerVoidConsumersHarness {
    private static int values, identities, states, failures;

    private interface Call { void run() throws Throwable; }

    private static final class TracedNumber extends Number {
        final Owner owner;
        int calls;
        RuntimeException failure;
        TracedNumber(Owner owner) { this.owner = owner; }
        public int intValue() {
            calls++;
            owner.events = owner.events * 10 + 3;
            if (failure != null) throw failure;
            return 17;
        }
        public long longValue() { throw new AssertionError("wrong root conversion"); }
        public float floatValue() { throw new AssertionError("wrong root conversion"); }
        public double doubleValue() { throw new AssertionError("wrong root conversion"); }
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Call[] paths(Owner.Receiver<?, ?> receiver, Object value, Method rootMethod) {
        RootAction root = receiver;
        return new Call[] {
            () -> root.accept(value),
            () -> {
                try {
                    if (rootMethod.invoke(receiver, value) != null)
                        throw new AssertionError("root void method returned a value");
                } catch (InvocationTargetException error) { throw error.getCause(); }
            }
        };
    }

    private static void execute(Call call, Owner owner, Owner.Receiver<?, ?> receiver,
                                TracedNumber number, int targetCalls, int conversions,
                                int next, int events, Class<? extends Throwable> errorKind,
                                RuntimeException sentinel) {
        int oldCalls = receiver.calls;
        int oldOuterCalls = owner.calls;
        int oldConversions = number.calls;
        int oldValue = receiver.value;
        int oldOuterValue = owner.value;
        Object oldReceiver = owner.lastReceiver;
        owner.events = 0;
        try {
            call.run();
            if (errorKind != null) throw new AssertionError("root consumer failure missing");
        } catch (Throwable error) {
            if (errorKind == null || error.getClass() != errorKind
                    || (sentinel != null && error != sentinel))
                throw new AssertionError("root consumer exception type or identity changed", error);
            if (sentinel != null) identities++;
            failures++;
        }
        if (receiver.calls != oldCalls + targetCalls || owner.calls != oldOuterCalls + targetCalls
                || number.calls != oldConversions + conversions || owner.events != events
                || receiver.value != (targetCalls == 0 ? oldValue : next)
                || owner.value != (targetCalls == 0 ? oldOuterValue : next))
            throw new AssertionError("root dispatch, conversion or captured effect order changed");
        if (owner.lastReceiver != (targetCalls == 0 ? oldReceiver : receiver))
            throw new AssertionError("root dispatch changed receiver identity");
        identities++;
        if (targetCalls != 0) values++;
        states++;
    }

    public static void main(String[] args) throws Exception {
        IntegerVoidConsumersHarness.main(args);
        Type[] edges = Action.class.getGenericInterfaces();
        Type variable = Action.class.getTypeParameters()[0];
        if (edges.length != 1 || !(edges[0] instanceof ParameterizedType)
                || ((ParameterizedType) edges[0]).getRawType() != RootAction.class
                || !((ParameterizedType) edges[0]).getActualTypeArguments()[0].equals(variable)
                || !Action.class.getDeclaredMethod("accept", Object.class)
                    .getGenericParameterTypes()[0].equals(variable))
            throw new AssertionError("complete direct-to-root interface scope changed");
        Method rootMethod = RootAction.class.getDeclaredMethod("accept", Object.class);
        if (!rootMethod.getGenericParameterTypes()[0].equals(RootAction.class.getTypeParameters()[0])
                || rootMethod.getReturnType() != void.class || !Modifier.isAbstract(rootMethod.getModifiers()))
            throw new AssertionError("immutable root declaration contract changed");
        Owner owner = new Owner();
        Owner.Receiver<Object, Object> receiver = owner.new Receiver<>();
        TracedNumber traced = new TracedNumber(owner);
        Number[] numbers = {Integer.valueOf(Integer.MIN_VALUE), Integer.valueOf(Integer.MAX_VALUE),
                            Long.valueOf(0x100000001L), Double.valueOf(-7.75), traced};
        int[] expected = {Integer.MIN_VALUE, Integer.MAX_VALUE, 1, -7, 17};
        for (int index = 0; index < numbers.length; index++) {
            boolean conversion = numbers[index] == traced;
            for (Call call : paths(receiver, numbers[index], rootMethod))
                execute(call, owner, receiver, traced, 1, conversion ? 1 : 0,
                        expected[index], conversion ? 31 : 1, null, null);
        }
        RuntimeException problem = new IllegalStateException("owned inherited consumer failure");
        traced.failure = problem;
        for (Call call : paths(receiver, traced, rootMethod))
            execute(call, owner, receiver, traced, 0, 1, 0, 3, IllegalStateException.class, problem);
        traced.failure = null;
        receiver.failure = problem;
        for (Call call : paths(receiver, traced, rootMethod))
            execute(call, owner, receiver, traced, 1, 1, 17, 31, IllegalStateException.class, problem);
        receiver.failure = null;
        for (Call call : paths(receiver, new Object(), rootMethod))
            execute(call, owner, receiver, traced, 0, 0, 0, 0, ClassCastException.class, null);
        for (Call call : paths(receiver, null, rootMethod))
            execute(call, owner, receiver, traced, 0, 0, 0, 0, NullPointerException.class, null);
        for (Call call : paths(null, traced, rootMethod))
            execute(call, owner, receiver, traced, 0, 0, 0, 0, NullPointerException.class, null);
        System.out.println("inherited-values=" + values + ",inherited-identities=" + identities
                + ",inherited-states=" + states + ",inherited-failures=" + failures);
        System.out.println("root-interface-dispatch=matched,parent-generic-scopes=unchanged,root-declaration=unchanged");
    }
}
