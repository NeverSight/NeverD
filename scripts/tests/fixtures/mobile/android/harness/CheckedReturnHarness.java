import fixture.Owner;
import fixture.AllocationState;
import fixture.Result;
import fixture.ReturnAdapter;
import fixture.Transform;
import java.lang.reflect.ParameterizedType;

public final class CheckedReturnHarness {
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("checked return behavior changed");
    }
    private static Object call(ReturnAdapter target, Owner<?> owner, boolean bridge) {
        return bridge ? ((Transform) target).apply(owner) : target.apply(owner);
    }
    public static void main(String[] args) throws Exception {
        ReturnAdapter target = new ReturnAdapter();
        Owner<Object> firstOwner = new Owner<Object>(new Object());
        firstOwner.ready = true;
        firstOwner.stamp = -77;
        AllocationState.owner = firstOwner;
        Result first = (Result) target.apply(firstOwner);
        require(first.token == firstOwner.value && first.stamp == Long.MAX_VALUE
                && firstOwner.reads == 1 && AllocationState.initializations == 1
                && AllocationState.readsDuringInitialization == 1);
        int constructions = Result.constructions;
        try { ReturnAdapter.make(null); throw new AssertionError("field receiver failure missing"); }
        catch (NullPointerException expected) { require(Result.constructions == constructions); }
        require(((Result) ReturnAdapter.make(firstOwner)).stamp == Long.MAX_VALUE
                && Result.constructions == constructions + 1);
        System.out.println("allocation-initialization-before-field-read=matched;null-field-receiver=matched");
        Object[] values = {null, "owned", Integer.valueOf(41), new Object()};
        long[] stamps = {Long.MIN_VALUE, -1, 0, Long.MAX_VALUE};
        for (boolean bridge : new boolean[] {false, true}) {
            for (int i = 0; i < values.length; ++i) {
                Owner<Object> owner = new Owner<Object>(values[i]);
                ReturnAdapter.calls = Result.constructions = 0;
                Result.failure = null;
                require(call(target, owner, bridge) == null && ReturnAdapter.calls == 1
                        && owner.reads == 0 && Result.constructions == 0);
                owner.ready = true;
                Result previous = null;
                for (int round = 0; round < 2; ++round) {
                    owner.value = values[(i + round) % values.length];
                    owner.stamp = stamps[(i + round) % stamps.length];
                    Result result = (Result) call(target, owner, bridge);
                    require(result != previous && result.token == owner.value && result.stamp == owner.stamp
                            && owner.reads == round + 1 && Result.constructions == round + 1);
                    previous = result;
                }
                owner.failure = new IllegalArgumentException("owned read sentinel");
                try { call(target, owner, bridge); throw new AssertionError("read failure missing"); }
                catch (RuntimeException actual) {
                    require(actual == owner.failure && ReturnAdapter.calls == 4 && owner.reads == 3
                            && Result.constructions == 2);
                }
                owner.failure = null;
                Result.failure = new IllegalStateException("owned constructor sentinel");
                try { call(target, owner, bridge); throw new AssertionError("constructor failure missing"); }
                catch (RuntimeException actual) {
                    require(actual == Result.failure && ReturnAdapter.calls == 5 && owner.reads == 4
                            && Result.constructions == 3);
                }
                Result.failure = null;
                System.out.println(bridge + ":" + i + ":calls=5;reads=4;constructors=3");
            }
            int calls = ReturnAdapter.calls;
            require(call(target, null, bridge) == null && ReturnAdapter.calls == calls + 1);
        }
        int calls = ReturnAdapter.calls;
        try { ((Transform) target).apply("invalid"); throw new AssertionError("bridge cast failure missing"); }
        catch (ClassCastException expected) { require(ReturnAdapter.calls == calls); }
        try { ((ReturnAdapter) null).apply(null); throw new AssertionError("typed null receiver failure missing"); }
        catch (NullPointerException expected) { require(ReturnAdapter.calls == calls); }
        try { ((Transform) null).apply("invalid"); throw new AssertionError("bridge null receiver failure missing"); }
        catch (NullPointerException expected) { require(ReturnAdapter.calls == calls); }
        var method = ReturnAdapter.class.getDeclaredMethod("apply", Owner.class);
        require(method.getReturnType() == Object.class);
        Class<?> expected = args[0].equals("original") ? Result.class : Object.class;
        require(method.getGenericReturnType() == expected);
        var contract = (ParameterizedType) ReturnAdapter.class.getGenericInterfaces()[0];
        require(contract.getActualTypeArguments()[1] == expected);
        require(((ParameterizedType) method.getGenericParameterTypes()[0]).getRawType() == Owner.class);
        System.out.println("runtime-return=Object;null-and-bridge-failures=matched;reflection=matched");
    }
}
