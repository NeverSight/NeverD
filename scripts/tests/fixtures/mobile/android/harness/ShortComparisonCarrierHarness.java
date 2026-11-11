import fixture.*;
import java.lang.reflect.*;
import java.util.function.IntSupplier;

public final class ShortComparisonCarrierHarness {
    private static int values, identities, failures;
    private static void value(boolean ok) {
        values++;
        if (!ok) throw new AssertionError("value " + values);
    }
    private static void identity(Object actual, Object expected) {
        identities++;
        if (actual != expected) throw new AssertionError("identity " + identities);
    }
    private static void reset() {
        Effects.unboxCalls = Effects.targetCalls = 0;
        Effects.lastSelf = null;
    }
    private static void failure(IntSupplier action, Class<?> type, RuntimeException sentinel,
                                int unbox, int target, Object self) {
        reset();
        try {
            action.getAsInt();
            throw new AssertionError("missing original comparison failure");
        } catch (RuntimeException actual) {
            failures++;
            identity(actual.getClass(), type);
            if (sentinel != null) identity(actual, sentinel);
            identity(Effects.lastSelf, self);
            value(Effects.unboxCalls == unbox);
            value(Effects.targetCalls == target);
        }
    }
    public static void main(String[] args) throws Exception {
        short[] inputs = {Short.MIN_VALUE, -1, 0, 1, Short.MAX_VALUE};
        for (short left : inputs) for (short right : inputs) {
            Carrier self = new Carrier(left), other = new Carrier(right);
            reset();
            value(Caller.narrow(self, right) == Integer.compare(left, right));
            value(Effects.unboxCalls == 0);
            value(Effects.targetCalls == 1);
            identity(Effects.lastSelf, self);
            reset();
            value(Caller.wide(self, other) == Integer.compare(left, right));
            value(Effects.unboxCalls == 1);
            value(Effects.targetCalls == 1);
            identity(Effects.lastSelf, self);
            value(self.value == left);
            value(other.value == right);
        }
        Carrier self = new Carrier((short) -1), other = new Carrier((short) 1);
        RuntimeException sentinel = new IllegalStateException("owned short carrier failure");
        Effects.unboxFailure = sentinel;
        failure(() -> Caller.wide(self, other), sentinel.getClass(), sentinel, 1, 0, null);
        Effects.unboxFailure = null;
        Effects.targetFailure = sentinel;
        failure(() -> Caller.wide(self, other), sentinel.getClass(), sentinel, 1, 1, self);
        failure(() -> Caller.narrow(self, (short) 1), sentinel.getClass(), sentinel, 0, 1, self);
        Effects.targetFailure = null;
        failure(() -> Caller.wide(self, new Object()), ClassCastException.class, null, 0, 0, null);
        failure(() -> Caller.wide(self, null), NullPointerException.class, null, 0, 0, null);
        failure(() -> Caller.wide(null, other), NullPointerException.class, null, 0, 0, null);
        failure(() -> Caller.wide(null, new Object()), NullPointerException.class, null, 0, 0, null);
        failure(() -> Caller.narrow(null, (short) 1), NullPointerException.class, null, 0, 0, null);
        ParameterizedType parent = (ParameterizedType) Carrier.class.getGenericSuperclass();
        identity(parent.getRawType(), Root.class);
        identity(parent.getActualTypeArguments()[0], Carrier.class);
        Type contract = Carrier.class.getGenericInterfaces()[0];
        value(args.length == 0 ? contract instanceof ParameterizedType : contract == Comparable.class);
        Method bridge = Carrier.class.getDeclaredMethod("compareTo", Object.class);
        identity(bridge.getReturnType(), int.class);
        identity(bridge.getGenericParameterTypes()[0], Object.class);
        value(bridge.isBridge() == (args.length == 0));
        identity(Carrier.class.getDeclaredMethod("read").getReturnType(), short.class);
        identity(Carrier.class.getDeclaredField("value").getType(), short.class);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("short-comparison-conversion-and-dispatch=matched,raw-source-contract-and-flags=reported");
    }
}
