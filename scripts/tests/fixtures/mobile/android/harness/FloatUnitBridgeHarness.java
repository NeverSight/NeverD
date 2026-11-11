import fixture.*;
import java.lang.reflect.*;

public class FloatUnitBridgeHarness {
    static int values, identities, failures;
    static void check(boolean value) { values++; if (!value) throw new AssertionError("value " + values); }
    static void same(Object a, Object b) { identities++; if (a != b) throw new AssertionError("identity " + identities); }
    static class OwnedNumber extends Number {
        int calls; RuntimeException failure;
        public float floatValue() {
            calls++; Trace.events = Trace.events * 10 + 3;
            if (failure != null) throw failure;
            return 1.5f;
        }
        public int intValue() { throw new AssertionError("wrong conversion"); }
        public long longValue() { throw new AssertionError("wrong conversion"); }
        public double doubleValue() { throw new AssertionError("wrong conversion"); }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    static Object invoke(int entry, Receiver owner, Object marker, Object number) {
        return entry == 0 ? owner.invoke(marker, number) : ((Function) owner).invoke(marker, number);
    }
    static void reset(Receiver owner, OwnedNumber number) {
        owner.calls = 0; owner.seen = null; number.calls = 0; Trace.events = 0;
    }
    static void effects(Receiver owner, OwnedNumber number, int events, int calls, int conversions) {
        check(Trace.events == events); check(owner.calls == calls); check(number.calls == conversions);
    }
    public static void main(String[] args) throws Exception {
        boolean rebuilt = args.length == 1 && args[0].equals("rebuilt");
        Receiver owner = new Receiver(); Marker marker = new Marker(new Object());
        OwnedNumber custom = new OwnedNumber();
        reset(owner, custom);
        owner.invoke(marker, 1.5f);
        effects(owner, custom, 1, 1, 0); same(owner.seen, marker);
        for (int entry = 0; entry < 2; entry++) {
            reset(owner, custom);
            try { invoke(entry, owner, "wrong marker", custom); throw new AssertionError("missing marker cast"); }
            catch (ClassCastException expected) { failures++; }
            effects(owner, custom, 0, 0, 0);
            try { invoke(entry, owner, marker, "wrong number"); throw new AssertionError("missing number cast"); }
            catch (ClassCastException expected) { failures++; }
            effects(owner, custom, 0, 0, 0);
            try { invoke(entry, owner, marker, null); throw new AssertionError("missing null conversion"); }
            catch (NullPointerException expected) { failures++; }
            effects(owner, custom, 0, 0, 0);
            RuntimeException conversion = new RuntimeException("owned conversion sentinel");
            custom.failure = conversion;
            try { invoke(entry, owner, marker, custom); throw new AssertionError("missing conversion sentinel"); }
            catch (RuntimeException failure) { failures++; same(failure, conversion); }
            effects(owner, custom, 3, 0, 1); custom.failure = null;
            reset(owner, custom);
            RuntimeException target = new RuntimeException("owned target sentinel"); owner.failure = target;
            try { invoke(entry, owner, marker, custom); throw new AssertionError("missing target sentinel"); }
            catch (RuntimeException failure) { failures++; same(failure, target); }
            effects(owner, custom, 31, 1, 1); same(owner.seen, marker); owner.failure = null;
            reset(owner, custom);
            try { invoke(entry, null, "wrong marker", custom); throw new AssertionError("missing null receiver"); }
            catch (NullPointerException expected) { failures++; }
            effects(owner, custom, 0, 0, 0);
        }
        reset(owner, custom);
        Object first = invoke(0, owner, marker, custom);
        effects(owner, custom, 312, 1, 1); same(first, kotlin.Unit.INSTANCE); same(owner.seen, marker);
        reset(owner, custom);
        Object repeated = invoke(1, owner, marker, custom);
        effects(owner, custom, 31, 1, 1); same(repeated, first); same(owner.seen, marker);
        Number[] numbers = {Float.intBitsToFloat(0), Float.intBitsToFloat(0x80000000), Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY, Float.intBitsToFloat(0x7fc00001), Float.intBitsToFloat(0x7fc01234), -1.75d, 16777217L, Integer.MIN_VALUE, custom};
        for (int entry = 0; entry < 2; entry++) for (Number number : numbers)
            for (Marker input : new Marker[] {marker, null}) {
                reset(owner, custom);
                Object result = invoke(entry, owner, input, number);
                float expected = number == custom ? 1.5f : number.floatValue();
                check(owner.calls == 1); check(Float.floatToRawIntBits(owner.value) == Float.floatToRawIntBits(expected));
                check(Trace.events == (number == custom ? 31 : 1));
                check(custom.calls == (number == custom ? 1 : 0));
                same(result, first); same(owner.seen, input);
            }
        reset(owner, custom); owner.invoke((Marker) null, 1.5f);
        effects(owner, custom, 1, 1, 0); same(owner.seen, null);
        Method bridge = Receiver.class.getDeclaredMethod("invoke", Object.class, Object.class);
        check(bridge.isBridge() == !rebuilt); check(bridge.isSynthetic() == !rebuilt);
        same(bridge.getGenericReturnType(), Object.class);
        check(Receiver.class.getTypeParameters().length == 0);
        same(Receiver.class.getInterfaces()[0], Function.class);
        check(Receiver.class.getDeclaredMethod("invoke", Marker.class, float.class).getGenericReturnType() == void.class);
        Type contract = Receiver.class.getGenericInterfaces()[0];
        check(rebuilt ? contract == Function.class : java.util.Arrays.equals(((ParameterizedType) contract).getActualTypeArguments(), new Type[] {Marker.class, Float.class, kotlin.Unit.class}));
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("numeric-conversion-and-Unit-initialization-order=matched,reflection-differences=measured");
    }
}
