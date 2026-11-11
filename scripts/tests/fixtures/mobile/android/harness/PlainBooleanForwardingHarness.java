import fixture.*;
import java.lang.reflect.*;

public class PlainBooleanForwardingHarness {
    static int values, identities, failures;
    static void check(boolean value) { values++; if (!value) throw new AssertionError("value " + values); }
    static void same(Object a, Object b) { identities++; if (a != b) throw new AssertionError("identity " + identities); }
    static class Child extends Counter {
        public boolean addImpl(Object value) { return !super.addImpl(value); }
    }
    static void reset(Api<Object> owner, boolean selected, RuntimeException failure) {
        if (owner instanceof FinalCounter) {
            FinalCounter finalOwner = (FinalCounter) owner;
            finalOwner.calls = 0; finalOwner.selected = selected; finalOwner.failure = failure; finalOwner.seen = null;
        } else {
            Counter counter = (Counter) owner;
            counter.calls = 0; counter.selected = selected; counter.failure = failure; counter.seen = null;
        }
    }
    static int calls(Api<Object> owner) { return owner instanceof FinalCounter ? ((FinalCounter) owner).calls : ((Counter) owner).calls; }
    static boolean selected(Api<Object> owner) { return owner instanceof FinalCounter ? ((FinalCounter) owner).selected : ((Counter) owner).selected; }
    static Object seen(Api<Object> owner) { return owner instanceof FinalCounter ? ((FinalCounter) owner).seen : ((Counter) owner).seen; }
    static boolean invoke(int entry, Api<Object> owner, Object value) {
        if (entry == 1) return owner.add(value);
        if (owner instanceof FinalCounter) return entry == 0 ? ((FinalCounter) owner).add(value) : ((FinalCounter) owner).addImpl(value);
        return entry == 0 ? ((Counter) owner).add(value) : ((Counter) owner).addImpl(value);
    }
    static void fails(Runnable run) {
        try { run.run(); throw new AssertionError("missing null receiver"); }
        catch (NullPointerException expected) { failures++; }
    }
    public static void main(String[] args) throws Exception {
        boolean rebuilt = args.length == 1 && args[0].equals("rebuilt");
        Api<Object>[] owners = new Api[] {new Counter(), new FinalCounter(), new Child()};
        Object[] inputs = {new Object(), "owned", null, new int[] {7}};
        for (Api<Object> owner : owners) for (int entry = 0; entry < 3; entry++)
            for (Object input : inputs) for (boolean choice : new boolean[] {false, true}) {
                reset(owner, choice, null);
                boolean result = invoke(entry, owner, input);
                check(result == (owner instanceof Child ? !choice : choice));
                check(calls(owner) == 1); check(selected(owner) == choice); same(seen(owner), input);
            }
        RuntimeException sentinel = new RuntimeException("owned forwarding failure");
        for (Api<Object> owner : owners) for (int entry = 0; entry < 3; entry++) {
            reset(owner, true, sentinel);
            try { invoke(entry, owner, inputs[0]); throw new AssertionError("missing target sentinel"); }
            catch (RuntimeException failure) { failures++; same(failure, sentinel); }
            check(calls(owner) == 1); check(selected(owner)); same(seen(owner), inputs[0]);
        }
        fails(() -> ((Counter) null).add(inputs[0]));
        fails(() -> ((Counter) null).addImpl(inputs[0]));
        fails(() -> ((FinalCounter) null).add(inputs[0]));
        fails(() -> ((FinalCounter) null).addImpl(inputs[0]));
        fails(() -> ((Api<Object>) null).add(inputs[0]));
        fails(() -> invoke(1, null, inputs[0]));
        for (Class<?> owner : new Class<?>[] {Counter.class, FinalCounter.class}) {
            Method bridge = owner.getDeclaredMethod("add", Object.class);
            check(bridge.isBridge() == !rebuilt); check(bridge.isSynthetic() == !rebuilt);
            check(bridge.getGenericReturnType() == boolean.class);
            ParameterizedType contract = (ParameterizedType) owner.getGenericInterfaces()[0];
            same(contract.getRawType(), Api.class);
            check(java.util.Arrays.equals(contract.getActualTypeArguments(), new Type[] {Object.class}));
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("plain-boolean-forwarding-and-virtual-dispatch=matched,full-generic-metadata=retained");
    }
}
