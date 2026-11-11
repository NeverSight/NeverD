import fixture.*;
import java.lang.reflect.*;

public class InheritedIntegerRangeCarriersHarness {
    static int values, identities, failures;
    static void check(boolean value) { values++; if (!value) throw new AssertionError("value " + values); }
    static void same(Object a, Object b) { identities++; if (a != b) throw new AssertionError("identity " + identities); }
    static class OwnedNumber extends Number {
        int calls; RuntimeException failure;
        public int intValue() { calls++; if (failure != null) throw failure; return -31; }
        public long longValue() { throw new AssertionError("wrong conversion"); }
        public float floatValue() { throw new AssertionError("wrong conversion"); }
        public double doubleValue() { throw new AssertionError("wrong conversion"); }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    static Object invoke(int entry, IntegerActor owner, Object[] args, Object tag, Object first, Object second) {
        if (entry == 0) return owner.invoke(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], tag, first, second);
        if (entry == 1) return ((IntegerFunction) owner).invoke(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], tag, first, second);
        return ((FixedIntegerFunction) (Object) owner).invoke(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], (IntegerTag) tag, (Integer) first, (Integer) second);
    }
    static void reset(IntegerActor owner, OwnedNumber first, OwnedNumber second) {
        owner.calls = 0; owner.last = null; owner.captured = null;
        first.calls = 0; second.calls = 0;
    }
    static void effects(IntegerActor owner, OwnedNumber first, OwnedNumber second, int a, int b, int calls) {
        check(first.calls == a); check(second.calls == b); check(owner.calls == calls);
    }
    public static void main(String[] args) throws Exception {
        boolean rebuilt = args.length == 1 && args[0].equals("rebuilt");
        IntegerActor owner = new IntegerActor();
        IntegerTag tag = new IntegerTag(7);
        Object[] arguments = {new Object(), "owned", null, new IntegerTag(9), new Object(), null, tag, "tail", new Object(), owner};
        OwnedNumber customFirst = new OwnedNumber(), customSecond = new OwnedNumber();
        Number[] firstNumbers = {Integer.MIN_VALUE, Integer.MAX_VALUE, 0, -1, 0x100000001L, 1.75f, customFirst};
        Number[] secondNumbers = {Integer.MIN_VALUE, Integer.MAX_VALUE, 0, -1, 0x100000001L, 1.75f, customSecond};
        Object[] selected = {new Object(), "owned result", null};
        for (int entry = 0; entry < 3; entry++) for (Number first : firstNumbers) for (Number second : secondNumbers) {
            if (entry == 2 && (!(first instanceof Integer) || !(second instanceof Integer))) continue;
            for (IntegerTag input : new IntegerTag[] {tag, null}) for (Object result : selected) {
                reset(owner, customFirst, customSecond); owner.selected = result;
                Object actual = invoke(entry, owner, arguments, input, first, second);
                same(actual, result); same(owner.selected, result); same(owner.last, input);
                for (int i = 0; i < arguments.length; i++) same(owner.captured[i], arguments[i]);
                check(owner.calls == 1);
                check(owner.firstMask == (first == customFirst ? -31 : first.intValue()));
                check(owner.secondMask == (second == customSecond ? -31 : second.intValue()));
                check(customFirst.calls == (first == customFirst ? 1 : 0));
                check(customSecond.calls == (second == customSecond ? 1 : 0));
            }
        }
        for (int entry = 0; entry < 2; entry++) {
            reset(owner, customFirst, customSecond);
            try { invoke(entry, owner, arguments, "wrong tag", customFirst, customSecond); throw new AssertionError("missing tag cast"); }
            catch (ClassCastException expected) { failures++; }
            effects(owner, customFirst, customSecond, 0, 0, 0);
            try { invoke(entry, owner, arguments, tag, "wrong first", customSecond); throw new AssertionError("missing first cast"); }
            catch (ClassCastException expected) { failures++; }
            effects(owner, customFirst, customSecond, 0, 0, 0);
            try { invoke(entry, owner, arguments, tag, null, customSecond); throw new AssertionError("missing first null"); }
            catch (NullPointerException expected) { failures++; }
            effects(owner, customFirst, customSecond, 0, 0, 0);
            RuntimeException firstFailure = new RuntimeException("owned first conversion");
            customFirst.failure = firstFailure;
            try { invoke(entry, owner, arguments, tag, customFirst, customSecond); throw new AssertionError("missing first sentinel"); }
            catch (RuntimeException failure) { failures++; same(failure, firstFailure); }
            effects(owner, customFirst, customSecond, 1, 0, 0); customFirst.failure = null;
            reset(owner, customFirst, customSecond);
            try { invoke(entry, owner, arguments, tag, customFirst, "wrong second"); throw new AssertionError("missing second cast"); }
            catch (ClassCastException expected) { failures++; }
            effects(owner, customFirst, customSecond, 1, 0, 0);
            reset(owner, customFirst, customSecond);
            try { invoke(entry, owner, arguments, tag, customFirst, null); throw new AssertionError("missing second null"); }
            catch (NullPointerException expected) { failures++; }
            effects(owner, customFirst, customSecond, 1, 0, 0);
            reset(owner, customFirst, customSecond);
            RuntimeException secondFailure = new RuntimeException("owned second conversion");
            customSecond.failure = secondFailure;
            try { invoke(entry, owner, arguments, tag, customFirst, customSecond); throw new AssertionError("missing second sentinel"); }
            catch (RuntimeException failure) { failures++; same(failure, secondFailure); }
            effects(owner, customFirst, customSecond, 1, 1, 0); customSecond.failure = null;
            reset(owner, customFirst, customSecond);
            RuntimeException target = new RuntimeException("owned target failure");
            owner.failure = target;
            try { invoke(entry, owner, arguments, tag, customFirst, customSecond); throw new AssertionError("missing target sentinel"); }
            catch (RuntimeException failure) { failures++; same(failure, target); }
            effects(owner, customFirst, customSecond, 1, 1, 1);
            same(owner.last, tag); check(owner.firstMask == -31); check(owner.secondMask == -31);
            for (int i = 0; i < arguments.length; i++) same(owner.captured[i], arguments[i]);
            owner.failure = null; reset(owner, customFirst, customSecond);
            try { invoke(entry, null, arguments, "wrong tag", customFirst, customSecond); throw new AssertionError("missing null receiver"); }
            catch (NullPointerException expected) { failures++; }
            effects(owner, customFirst, customSecond, 0, 0, 0);
        }
        Method bridge = IntegerActor.class.getDeclaredMethod("invoke", Object.class, Object.class, Object.class, Object.class, Object.class, Object.class, Object.class, Object.class, Object.class, Object.class, Object.class, Object.class, Object.class);
        check(bridge.isBridge() == !rebuilt); check(bridge.isSynthetic() == !rebuilt);
        same(bridge.getGenericReturnType(), Object.class);
        check(IntegerActor.class.getTypeParameters().length == 0);
        same(IntegerActor.class.getInterfaces()[0], FixedIntegerFunction.class);
        Type parent = FixedIntegerFunction.class.getGenericInterfaces()[0];
        if (rebuilt) {
            same(parent, IntegerFunction.class); check(!(parent instanceof ParameterizedType));
        } else {
            ParameterizedType contract = (ParameterizedType) parent;
            same(contract.getRawType(), IntegerFunction.class);
            check(java.util.Arrays.equals(contract.getActualTypeArguments(), new Type[] {Object.class, Object.class, Object.class, Object.class, Object.class, Object.class, Object.class, Object.class, Object.class, Object.class, IntegerTag.class, Integer.class, Integer.class, Object.class}));
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("inherited-integer-carrier-scope=measured,conversion-dispatch-identity-and-failure-order=matched");
    }
}
