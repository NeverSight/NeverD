import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class LongComparisonCarrierHarness {
    static int values, identities, states, failures;
    static final Method erased, parent;
    static final MethodHandle erasedHandle, parentHandle;
    static {
        try {
            erased = Carrier.class.getMethod("compareTo", Object.class);
            parent = Comparable.class.getMethod("compareTo", Object.class);
            erasedHandle = MethodHandles.publicLookup().unreflect(erased);
            parentHandle = MethodHandles.publicLookup().unreflect(parent);
        } catch (Exception error) { throw new AssertionError(error); }
    }
    static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original carrier alias changed");
        identities++;
    }
    static void state(long actual, long expected) {
        if (actual != expected) throw new AssertionError("wide input bits or callback effects changed");
        states++;
    }
    static void reset() {
        Effects.unboxCalls = Effects.targetCalls = Effects.primitiveCalls = 0;
        Effects.lastInput = Effects.lastLeft = Effects.lastRight = 0;
        Effects.lastSelf = Effects.lastUnboxed = null;
    }
    static int reflected(Method method, Carrier self, Object other) throws Throwable {
        try { return ((Integer) method.invoke(self, other)).intValue(); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static int call(int route, Carrier self, Object other) throws Throwable {
        switch (route) {
        case 0: return Caller.narrow(self, ((Carrier) other).value);
        case 1: return Caller.wide(self, other);
        case 2: return self.compareTo(other);
        case 3: return ((Comparable) self).compareTo(other);
        case 4: return reflected(erased, self, other);
        case 5: return reflected(parent, self, other);
        case 6: return (int) erasedHandle.invoke(self, other);
        default: return (int) parentHandle.invoke(self, other);
        }
    }
    static void compare(int route, Carrier self, Carrier other) throws Throwable {
        long left = self.value, right = other.value;
        reset();
        if (call(route, self, other) != Long.compare(left, right)) throw new AssertionError("long comparison changed");
        values++;
        state(Effects.unboxCalls, route == 0 ? 0 : 1);
        state(Effects.targetCalls, 1); state(Effects.primitiveCalls, 1);
        state(Effects.lastInput, right); state(Effects.lastLeft, left); state(Effects.lastRight, right);
        state(self.value, left); state(other.value, right);
        identity(Effects.lastSelf, self); identity(Effects.lastUnboxed, route == 0 ? null : other);
    }
    interface Run { void call() throws Throwable; }
    static void failure(Run action, Class<?> type, Throwable original, int unbox, int target,
                        int primitive, Object self, Object converted) throws Throwable {
        reset();
        try { action.call(); throw new AssertionError("missing original failure"); }
        catch (Throwable error) {
            if (error.getClass() != type) throw new AssertionError("failure order changed", error);
            if (original != null) identity(error, original);
            failures++;
        }
        state(Effects.unboxCalls, unbox); state(Effects.targetCalls, target); state(Effects.primitiveCalls, primitive);
        identity(Effects.lastSelf, self); identity(Effects.lastUnboxed, converted);
    }
    public static void main(String[] args) throws Throwable {
        long[] inputs = {Long.MIN_VALUE, -4294967296L, -1L, 0L, 1L, 4294967295L,
                         4294967296L, 0x0102030405060708L, Long.MAX_VALUE};
        for (int route = 0; route < 8; route++) {
            for (long left : inputs) {
                Carrier self = new Carrier(left);
                for (long right : inputs) compare(route, self, new Carrier(right));
                compare(route, self, self);
            }
            final int selected = route;
            Carrier self = new Carrier(Long.MIN_VALUE), other = new Carrier(0x0102030405060708L);
            RuntimeException problem = new IllegalStateException("owned long carrier failure");
            int unbox = route == 0 ? 0 : 1;
            Object converted = route == 0 ? null : other;
            if (route != 0) {
                Effects.unboxFailure = problem;
                failure(() -> call(selected, self, other), problem.getClass(), problem, 1, 0, 0, null, other);
                Effects.unboxFailure = null;
            }
            Effects.targetFailure = problem;
            failure(() -> call(selected, self, other), problem.getClass(), problem, unbox, 1, 0, self, converted);
            Effects.targetFailure = null; Effects.primitiveFailure = problem;
            failure(() -> call(selected, self, other), problem.getClass(), problem, unbox, 1, 1, self, converted);
            Effects.primitiveFailure = null;
            failure(() -> call(selected, null, other), NullPointerException.class, null, 0, 0, 0, null, null);
            if (route != 0) {
                failure(() -> call(selected, self, null), NullPointerException.class, null, 0, 0, 0, null, null);
                failure(() -> call(selected, self, new Object()), ClassCastException.class, null, 0, 0, 0, null, null);
                failure(() -> call(selected, null, new Object()), NullPointerException.class, null, 0, 0, 0, null, null);
            }
        }
        ParameterizedType parentType = (ParameterizedType) Carrier.class.getGenericSuperclass();
        if (parentType.getRawType() != Root.class || parentType.getActualTypeArguments()[0] != Carrier.class)
            throw new AssertionError("unrelated generic superclass changed");
        Type contract = Carrier.class.getGenericInterfaces()[0];
        if (args.length == 0 ? !(contract instanceof ParameterizedType) : contract != Comparable.class)
            throw new AssertionError("raw source interface differs from report");
        if (erased.isBridge() != (args.length == 0) || erased.isSynthetic() != (args.length == 0)
                || Carrier.class.getMethod("read").getReturnType() != long.class)
            throw new AssertionError("original conversion ABI or bridge flags changed beyond report");
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("long-comparison-both-words-aliases-and-effects=matched,raw-source-contract-and-flags=reported");
    }
}
