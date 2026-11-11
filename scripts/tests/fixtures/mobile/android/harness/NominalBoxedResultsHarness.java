import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class NominalBoxedResultsHarness {
    static int values, identities, states, failures;
    static Method narrow, broad, scalar;
    static MethodHandle narrowHandle, broadHandle, scalarHandle, parentHandle;
    static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original boxed identity changed");
        identities++;
    }
    static void state(long actual, long expected) {
        if (actual != expected) throw new AssertionError("scalar words or original effects changed");
        states++;
    }
    static void reset() {
        Effects.scalarCalls = Effects.boxCalls = Effects.events = 0;
        Effects.lastDelta = Effects.lastScalar = Effects.lastBoxInput = 0;
        Effects.lastSelf = Effects.lastBox = null;
    }
    static Object reflected(Method method, Core receiver, long delta) throws Throwable {
        try { return method.invoke(receiver, delta); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static Parent boxed(int route, Core self, long delta) throws Throwable {
        switch (route) {
        case 0: return Caller.typed(self, delta);
        case 1: return Caller.parent(self, delta);
        case 2: return self.shift(delta);
        case 3: return (Parent) reflected(narrow, self, delta);
        case 4: return (Parent) reflected(broad, self, delta);
        case 5: return (Parent) narrowHandle.invoke(self, delta);
        case 6: return (Parent) broadHandle.invoke(self, delta);
        default: return (Parent) parentHandle.invoke(self, delta);
        }
    }
    static long primitive(int route, Core self, long delta) throws Throwable {
        if (route == 0) return Caller.scalar(self, delta);
        if (route == 1) return ((Long) reflected(scalar, self, delta)).longValue();
        return (long) scalarHandle.invoke(self, delta);
    }
    interface Run { void call() throws Throwable; }
    static void failure(Run action, Class<?> type, Throwable original, int target, int box, int events, Core self) throws Throwable {
        reset();
        try { action.call(); throw new AssertionError("missing original failure"); }
        catch (Throwable error) {
            if (error.getClass() != type) throw new AssertionError("failure order changed", error);
            if (original != null) identity(error, original);
            failures++;
        }
        state(Effects.scalarCalls, target); state(Effects.boxCalls, box); state(Effects.events, events);
        identity(Effects.lastSelf, self); identity(Effects.lastBox, null);
    }
    public static void main(String[] args) throws Throwable {
        boolean original = args[0].equals("original");
        for (Method method : Core.class.getDeclaredMethods()) {
            if (method.getName().equals("shift") && method.getReturnType() == Api.class) narrow = method;
            if (method.getName().equals("shift") && method.getReturnType() == Parent.class) broad = method;
            if (method.getName().equals(args[1]) && method.getReturnType() == long.class) scalar = method;
        }
        if (narrow == null || broad == null || scalar == null || narrow.isBridge() != original
                || narrow.isSynthetic() != original || !broad.isBridge() || !broad.isSynthetic()
                || !Modifier.isFinal(scalar.getModifiers()))
            throw new AssertionError("reported boxed return slots or scalar alias differ");
        narrowHandle = MethodHandles.publicLookup().unreflect(narrow);
        broadHandle = MethodHandles.publicLookup().unreflect(broad);
        scalarHandle = MethodHandles.publicLookup().unreflect(scalar);
        parentHandle = MethodHandles.publicLookup().unreflect(Parent.class.getMethod("shift", long.class));
        long[] bases = {Long.MIN_VALUE, -1L, 0L, 0x0102030405060708L, Long.MAX_VALUE};
        long[] deltas = {Long.MIN_VALUE, -4294967296L, -1L, 0L, 1L, 4294967295L, Long.MAX_VALUE};
        for (int route = 0; route < 8; route++) {
            for (long base : bases) for (long delta : deltas) for (boolean nil : new boolean[]{false, true}) {
                Core self = new Core(base); Effects.returnNull = nil; reset();
                Parent result = boxed(route, self, delta);
                if (nil ? result != null : !(result instanceof Core) || result == self || ((Core) result).value != base + delta)
                    throw new AssertionError("original allocation, null result or wide addition changed");
                values++;
                identity(Effects.lastSelf, self); identity(result, Effects.lastBox);
                state(Effects.scalarCalls, 1); state(Effects.boxCalls, 1); state(Effects.events, 12);
                state(Effects.lastDelta, delta); state(Effects.lastScalar, base + delta);
                state(Effects.lastBoxInput, base + delta); state(self.value, base);
            }
            Effects.returnNull = false;
            Core self = new Core(7); final int selected = route;
            RuntimeException problem = new IllegalStateException("owned nominal boxing failure");
            Effects.scalarFailure = problem;
            failure(() -> boxed(selected, self, 0x0102030405060708L), problem.getClass(), problem, 1, 0, 1, self);
            Effects.scalarFailure = null; Effects.boxFailure = problem;
            failure(() -> boxed(selected, self, 0x0102030405060708L), problem.getClass(), problem, 1, 1, 12, self);
            Effects.boxFailure = null;
            failure(() -> boxed(selected, null, 3), NullPointerException.class, null, 0, 0, 0, null);
        }
        for (int route = 0; route < 3; route++) {
            for (long base : bases) for (long delta : deltas) {
                Core self = new Core(base); reset();
                if (primitive(route, self, delta) != base + delta) throw new AssertionError("original scalar slot changed");
                values++; identity(Effects.lastSelf, self);
                state(Effects.scalarCalls, 1); state(Effects.boxCalls, 0); state(Effects.events, 1);
                state(Effects.lastDelta, delta); state(Effects.lastScalar, base + delta); state(self.value, base);
            }
            Core self = new Core(7); final int selected = route;
            RuntimeException problem = new IllegalStateException("owned original scalar failure");
            Effects.scalarFailure = problem;
            failure(() -> primitive(selected, self, 3), problem.getClass(), problem, 1, 0, 1, self);
            Effects.scalarFailure = null;
            failure(() -> primitive(selected, null, 3), NullPointerException.class, null, 0, 0, 0, null);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-scalar-boxing-null-allocation-and-failures=matched,alias-and-bridge-flags=reported");
    }
}
