import fixture.*;
import java.lang.reflect.*;

public final class IntegerVoidConsumersHarness {
    private static int values, identities, states, failures;

    private interface Call { Object run() throws Throwable; }

    private static final class TracedNumber extends Number {
        final Owner owner;
        final int value;
        int calls;
        RuntimeException failure;
        TracedNumber(Owner owner, int value) { this.owner = owner; this.value = value; }
        public int intValue() {
            calls++;
            owner.events = owner.events * 10 + 3;
            if (failure != null) throw failure;
            return value;
        }
        public long longValue() { throw new AssertionError("wrong Number conversion"); }
        public float floatValue() { throw new AssertionError("wrong Number conversion"); }
        public double doubleValue() { throw new AssertionError("wrong Number conversion"); }
    }

    private static Object reflect(Method method, Object receiver, Object value) throws Throwable {
        try { return method.invoke(receiver, value); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }

    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original captured identity changed");
        identities++;
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Call[] erased(Owner.Receiver<?, ?> receiver, Object value, Method bridge) {
        Action action = receiver;
        return new Call[] {
            () -> { receiver.accept(value); return null; },
            () -> { action.accept(value); return null; },
            () -> reflect(bridge, receiver, value)
        };
    }

    private static Call[] primitive(Owner.Receiver<?, ?> receiver, int value, Method target) {
        return new Call[] {
            () -> { receiver.accept(value); return null; },
            () -> reflect(target, receiver, Integer.valueOf(value))
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
            Object result = call.run();
            if (errorKind != null) throw new AssertionError("consumer failure missing");
            if (result != null) throw new AssertionError("void consumer returned a value");
        } catch (Throwable error) {
            if (errorKind == null || error.getClass() != errorKind
                    || (sentinel != null && error != sentinel))
                throw new AssertionError("consumer exception type or identity changed", error);
            if (sentinel != null) identities++;
            failures++;
        }
        if (receiver.calls != oldCalls + targetCalls || owner.calls != oldOuterCalls + targetCalls
                || number.calls != oldConversions + conversions || owner.events != events
                || receiver.value != (targetCalls == 0 ? oldValue : next)
                || owner.value != (targetCalls == 0 ? oldOuterValue : next))
            throw new AssertionError("Number conversion, target dispatch or captured effect order changed");
        same(owner.lastReceiver, targetCalls == 0 ? oldReceiver : receiver);
        if (targetCalls != 0) values++;
        states++;
    }

    public static void main(String[] args) throws Exception {
        boolean original = args[0].equals("original");
        Owner owner = new Owner();
        Owner.Receiver<Object, Object> receiver = owner.new Receiver<>();
        Method target = Owner.Receiver.class.getDeclaredMethod("accept", int.class);
        Method bridge = Owner.Receiver.class.getDeclaredMethod("accept", Object.class);
        if (target.getReturnType() != void.class || bridge.getReturnType() != void.class
                || !Modifier.isFinal(target.getModifiers()) || target.isBridge() || target.isSynthetic()
                || bridge.isBridge() != original || bridge.isSynthetic() != original)
            throw new AssertionError("original primitive or erased entry flags changed");
        if (Modifier.isStatic(Owner.Receiver.class.getModifiers()) == original
                || Owner.Receiver.class.getDeclaringClass() != Owner.class)
            throw new AssertionError("reported captured member projection changed");
        Field capture = Owner.Receiver.class.getDeclaredField("this$0");
        capture.setAccessible(true);
        same(capture.get(receiver), owner);
        if (capture.isSynthetic() != original)
            throw new AssertionError("reported captured field modifier difference changed");
        Type contract = Owner.Receiver.class.getGenericInterfaces()[0];
        if (original) {
            if (!(contract instanceof ParameterizedType)
                    || ((ParameterizedType) contract).getRawType() != Action.class
                    || ((ParameterizedType) contract).getActualTypeArguments()[0] != Integer.class)
                throw new AssertionError("original complete Integer consumer contract changed");
        } else if (contract != Action.class) {
            throw new AssertionError("reported raw Action interface missing");
        }
        if (Owner.Receiver.class.getTypeParameters().length != 2
                || !Owner.Receiver.class.getTypeParameters()[0].getName().equals("S")
                || !Owner.Receiver.class.getTypeParameters()[1].getName().equals("V"))
            throw new AssertionError("unrelated receiver generic scope changed");

        TracedNumber traced = new TracedNumber(owner, 17);
        Number[] numbers = {Integer.valueOf(0), Integer.valueOf(-1), Integer.valueOf(Integer.MIN_VALUE),
            Integer.valueOf(Integer.MAX_VALUE), Long.valueOf(0x100000001L), Double.valueOf(17.75),
            Double.valueOf(-17.75), Float.valueOf(Float.NaN), Double.valueOf(Double.POSITIVE_INFINITY), traced};
        int[] expected = {0, -1, Integer.MIN_VALUE, Integer.MAX_VALUE, 1, 17, -17, 0, Integer.MAX_VALUE, 17};
        for (int index = 0; index < numbers.length; index++) {
            for (Call call : primitive(receiver, expected[index], target))
                execute(call, owner, receiver, traced, 1, 0, expected[index], 1, null, null);
            boolean conversion = numbers[index] == traced;
            for (Call call : erased(receiver, numbers[index], bridge))
                execute(call, owner, receiver, traced, 1, conversion ? 1 : 0,
                        expected[index], conversion ? 31 : 1, null, null);
        }
        RuntimeException problem = new IllegalStateException("owned Integer consumer failure");
        traced.failure = problem;
        for (Call call : erased(receiver, traced, bridge))
            execute(call, owner, receiver, traced, 0, 1, 0, 3, IllegalStateException.class, problem);
        traced.failure = null;
        receiver.failure = problem;
        for (Call call : primitive(receiver, 17, target))
            execute(call, owner, receiver, traced, 1, 0, 17, 1, IllegalStateException.class, problem);
        for (Call call : erased(receiver, traced, bridge))
            execute(call, owner, receiver, traced, 1, 1, 17, 31, IllegalStateException.class, problem);
        receiver.failure = null;
        for (Object invalid : new Object[] {new Object(), "wrong", new Object[0]}) {
            for (Call call : erased(receiver, invalid, bridge))
                execute(call, owner, receiver, traced, 0, 0, 0, 0, ClassCastException.class, null);
        }
        for (Call call : erased(receiver, null, bridge))
            execute(call, owner, receiver, traced, 0, 0, 0, 0, NullPointerException.class, null);
        for (Call call : primitive(null, 17, target))
            execute(call, owner, receiver, traced, 0, 0, 0, 0, NullPointerException.class, null);
        for (Call call : erased(null, traced, bridge))
            execute(call, owner, receiver, traced, 0, 0, 0, 0, NullPointerException.class, null);
        System.out.println("values=" + values + ",identities=" + identities
                + ",states=" + states + ",failures=" + failures);
        System.out.println(original ? "reflection=original"
                : "reflection=reported-raw-interface-and-captured-member");
    }
}
