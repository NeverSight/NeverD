import fixture.*;
import java.lang.reflect.*;

public class InheritedIntegerCarriersHarness {
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
    static Object invoke(int entry, IntegerActor owner, Object first, Object second) {
        if (entry == 0) return owner.invoke(first, second);
        if (entry == 1) return ((IntegerFunction) owner).invoke(first, second);
        return ((FixedIntegerFunction) (Object) owner).invoke((IntegerTag) first, (Integer) second);
    }
    public static void main(String[] args) throws Exception {
        boolean rebuilt = args.length == 1 && args[0].equals("rebuilt");
        IntegerActor owner = new IntegerActor();
        IntegerTag tag = new IntegerTag(7);
        OwnedNumber custom = new OwnedNumber();
        Number[] numbers = {Integer.MIN_VALUE, Integer.MAX_VALUE, 0, -1, 0x100000001L, 1.75f, custom};
        Object[] selected = {new Object(), "owned", null};
        for (int entry = 0; entry < 3; entry++) for (Number number : numbers) {
            if (entry == 2 && !(number instanceof Integer)) continue;
            for (IntegerTag first : new IntegerTag[] {tag, null}) for (Object result : selected) {
                owner.calls = 0; owner.selected = result; owner.last = null; custom.calls = 0;
                Object actual = invoke(entry, owner, first, number);
                same(actual, result); same(owner.selected, result); same(owner.last, first);
                check(owner.calls == 1);
                check(owner.mask == (number == custom ? -31 : number.intValue()));
                check(custom.calls == (number == custom ? 1 : 0));
            }
        }
        for (int entry = 0; entry < 2; entry++) {
            custom.calls = 0; owner.calls = 0;
            try { invoke(entry, owner, "wrong tag", custom); throw new AssertionError("missing first cast"); }
            catch (ClassCastException expected) { failures++; }
            check(custom.calls == 0); check(owner.calls == 0);
            try { invoke(entry, owner, tag, "wrong number"); throw new AssertionError("missing number cast"); }
            catch (ClassCastException expected) { failures++; }
            check(owner.calls == 0);
            try { invoke(entry, owner, tag, null); throw new AssertionError("missing null conversion"); }
            catch (NullPointerException expected) { failures++; }
            check(owner.calls == 0);
            RuntimeException conversion = new RuntimeException("owned conversion failure");
            custom.failure = conversion; custom.calls = 0;
            try { invoke(entry, owner, tag, custom); throw new AssertionError("missing conversion sentinel"); }
            catch (RuntimeException failure) { failures++; same(failure, conversion); }
            check(custom.calls == 1); check(owner.calls == 0); custom.failure = null;
            RuntimeException target = new RuntimeException("owned target failure");
            owner.failure = target; owner.calls = 0; custom.calls = 0;
            try { invoke(entry, owner, tag, custom); throw new AssertionError("missing target sentinel"); }
            catch (RuntimeException failure) { failures++; same(failure, target); }
            check(custom.calls == 1); check(owner.calls == 1); same(owner.last, tag); check(owner.mask == -31);
            owner.failure = null; owner.calls = 0; custom.calls = 0;
            try { invoke(entry, null, "wrong tag", custom); throw new AssertionError("missing null receiver"); }
            catch (NullPointerException expected) { failures++; }
            check(custom.calls == 0); check(owner.calls == 0);
        }
        Method bridge = IntegerActor.class.getDeclaredMethod("invoke", Object.class, Object.class);
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
            check(java.util.Arrays.equals(contract.getActualTypeArguments(), new Type[] {IntegerTag.class, Integer.class, Object.class}));
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("inherited-integer-carrier-scope=measured,conversion-dispatch-identity-and-failure-order=matched");
    }
}
