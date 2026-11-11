import fixture.Function;
import fixture.Marker;
import fixture.Receiver;
import fixture.Trace;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;

public class IntegerUnitBridgeHarness {
    static int identities;
    static int failures;
    static void check(boolean condition) {
        if (!condition) throw new AssertionError();
    }
    static void same(Object actual, Object expected) {
        check(actual == expected);
        ++identities;
    }
    static class TracedNumber extends Number {
        int value;
        int calls;
        RuntimeException failure;
        TracedNumber(int value) { this.value = value; }
        public int intValue() {
            Trace.events = Trace.events * 10 + 3;
            ++calls;
            if (failure != null) throw failure;
            return value;
        }
        public long longValue() { throw new AssertionError(); }
        public float floatValue() { throw new AssertionError(); }
        public double doubleValue() { throw new AssertionError(); }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Receiver receiver = new Receiver();
        Function raw = receiver;
        Marker marker = new Marker(new Object());
        TracedNumber traced = new TracedNumber(17);
        RuntimeException sentinel = new IllegalStateException();
        check(Trace.events == 0);
        receiver.invoke(marker, 1);
        receiver.invoke(null, -1);
        check(Trace.events == 11 && receiver.calls == 2 && traced.calls == 0);
        traced.failure = sentinel;
        try {
            raw.invoke(marker, traced);
            throw new AssertionError();
        } catch (RuntimeException actual) { same(actual, sentinel); ++failures; }
        check(Trace.events == 113 && receiver.calls == 2 && traced.calls == 1);
        traced.failure = null;
        receiver.failure = sentinel;
        try {
            raw.invoke(marker, traced);
            throw new AssertionError();
        } catch (RuntimeException actual) { same(actual, sentinel); ++failures; }
        check(Trace.events == 11331 && receiver.calls == 3 && traced.calls == 2);
        receiver.failure = null;
        Object unit = raw.invoke(marker, traced);
        check(Trace.events == 11331312 && receiver.calls == 4 && traced.calls == 3);
        same(unit, kotlin.Unit.INSTANCE);
        same(raw.invoke(marker, traced), unit);
        check(Trace.events == 1133131231 && receiver.calls == 5 && traced.calls == 4);
        Trace.events = 0;
        for (Number value : new Number[] {Integer.valueOf(0), Integer.valueOf(-1),
                Integer.valueOf(Integer.MIN_VALUE), Integer.valueOf(Integer.MAX_VALUE),
                Long.valueOf(0x100000001L), Double.valueOf(17.75), Double.valueOf(-17.75), traced}) {
            Trace.events = 0;
            int calls = receiver.calls;
            int unboxes = traced.calls;
            same(raw.invoke(marker, value), unit);
            same(receiver.seen, marker);
            check(receiver.calls == calls + 1);
            int expected = value == traced ? 17 : value.intValue();
            check(receiver.value == expected);
            check(traced.calls == unboxes + (value == traced ? 1 : 0));
        }
        for (Object invalid : new Object[] {new Object(), "wrong", new Object[0]}) {
            int calls = receiver.calls;
            int unboxes = traced.calls;
            try {
                raw.invoke(invalid, traced);
                throw new AssertionError();
            } catch (ClassCastException expected) { ++failures; }
            check(receiver.calls == calls && traced.calls == unboxes);
            try {
                raw.invoke(marker, invalid);
                throw new AssertionError();
            } catch (ClassCastException expected) { ++failures; }
            check(receiver.calls == calls && traced.calls == unboxes);
        }
        int calls = receiver.calls;
        try {
            raw.invoke(marker, null);
            throw new AssertionError();
        } catch (NullPointerException expected) { ++failures; }
        check(receiver.calls == calls);
        same(raw.invoke(null, traced), unit);
        check(receiver.seen == null);
        marker.payload = unit;
        same(raw.invoke(marker, traced), unit);
        same(receiver.seen.payload, unit);
        try {
            ((Function) null).invoke(marker, traced);
            throw new AssertionError();
        } catch (NullPointerException expected) { ++failures; }
        Method typed = Receiver.class.getDeclaredMethod("invoke", Marker.class, int.class);
        Method bridge = Receiver.class.getDeclaredMethod("invoke", Object.class, Object.class);
        check(typed.getReturnType() == void.class && bridge.getReturnType() == Object.class);
        boolean original = args[0].equals("original");
        check(bridge.isBridge() == original && bridge.isSynthetic() == original);
        check((Receiver.class.getGenericInterfaces()[0] instanceof ParameterizedType) == original);
        if (original)
            check(Receiver.class.getGenericInterfaces()[0].getTypeName()
                    .equals("fixture.Function<fixture.Marker, java.lang.Integer, kotlin.Unit>"));
        else check(Receiver.class.getGenericInterfaces()[0] == Function.class);
        System.out.println("identities=" + identities + ",failures=" + failures + ",number-dispatch=matched");
        System.out.println("reflection=" + (original ? "original" : "reported-raw-interface"));
    }
}
