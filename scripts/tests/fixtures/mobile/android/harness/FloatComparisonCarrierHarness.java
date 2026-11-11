import fixture.*;
import java.lang.reflect.*;
import java.util.function.IntSupplier;

public final class FloatComparisonCarrierHarness {
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
        Effects.unboxCalls = Effects.targetCalls = Effects.primitiveCalls = 0;
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
            value(Effects.primitiveCalls == 0);
        }
    }
    public static void main(String[] args) throws Exception {
        int[] bits = {0xff800000, 0xff7fffff, 0xbf800000, 0x80000001,
                0x80000000, 0, 1, 0x3f800000, 0x7f7fffff, 0x7f800000,
                0x7fc00001, 0x7f800001, 0xffc00003, 0xff800005};
        float[] inputs = new float[bits.length];
        for (int i = 0; i < bits.length; ++i) inputs[i] = Float.intBitsToFloat(bits[i]);
        for (boolean collapse : new boolean[] {false, true}) {
            Effects.collapseNaN = collapse;
            for (float left : inputs) for (float right : inputs) {
                int expected = collapse && (Float.isNaN(left) || Float.isNaN(right)) ?
                        0 : Float.compare(left, right);
                Carrier self = new Carrier(left), other = new Carrier(right);
                reset();
                value(Caller.narrow(self, right) == expected);
                value(Effects.unboxCalls == 0);
                value(Effects.targetCalls == 1);
                value(Effects.primitiveCalls == 1);
                identity(Effects.lastSelf, self);
                reset();
                value(Caller.wide(self, other) == expected);
                value(Effects.unboxCalls == 1);
                value(Effects.targetCalls == 1);
                value(Effects.primitiveCalls == 1);
                identity(Effects.lastSelf, self);
                value(Float.floatToRawIntBits(self.value) == Float.floatToRawIntBits(left));
                value(Float.floatToRawIntBits(other.value) == Float.floatToRawIntBits(right));
            }
        }
        Effects.collapseNaN = false;
        Carrier self = new Carrier(-1.0f), other = new Carrier(1.0f);
        RuntimeException sentinel = new IllegalStateException("owned float carrier failure");
        Effects.unboxFailure = sentinel;
        failure(() -> Caller.wide(self, other), sentinel.getClass(), sentinel, 1, 0, null);
        Effects.unboxFailure = null;
        Effects.targetFailure = sentinel;
        failure(() -> Caller.wide(self, other), sentinel.getClass(), sentinel, 1, 1, self);
        failure(() -> Caller.narrow(self, 1.0f), sentinel.getClass(), sentinel, 0, 1, self);
        Effects.targetFailure = null;
        failure(() -> Caller.wide(self, new Object()), ClassCastException.class, null, 0, 0, null);
        failure(() -> Caller.wide(self, null), NullPointerException.class, null, 0, 0, null);
        failure(() -> Caller.wide(null, other), NullPointerException.class, null, 0, 0, null);
        failure(() -> Caller.wide(null, new Object()), NullPointerException.class, null, 0, 0, null);
        failure(() -> Caller.narrow(null, 1.0f), NullPointerException.class, null, 0, 0, null);
        ParameterizedType parent = (ParameterizedType) Carrier.class.getGenericSuperclass();
        identity(parent.getRawType(), Root.class);
        identity(parent.getActualTypeArguments()[0], Carrier.class);
        Type contract = Carrier.class.getGenericInterfaces()[0];
        value(args.length == 0 ? contract instanceof ParameterizedType : contract == Comparable.class);
        Method bridge = Carrier.class.getDeclaredMethod("compareTo", Object.class);
        identity(bridge.getReturnType(), int.class);
        identity(bridge.getGenericParameterTypes()[0], Object.class);
        value(bridge.isBridge() == (args.length == 0));
        identity(Carrier.class.getDeclaredMethod("read").getReturnType(), float.class);
        identity(Carrier.class.getDeclaredField("value").getType(), float.class);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("float-comparison-conversion-and-dispatch=matched,raw-source-contract-and-flags=reported");
    }
}
