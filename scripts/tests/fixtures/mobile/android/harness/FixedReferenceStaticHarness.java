import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class FixedReferenceStaticHarness {
    static int values, identities, states, failures;
    static void value(long actual, long expected) {
        if (actual != expected) throw new AssertionError("wide input bits changed");
        values++;
    }
    static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("covariant result identity changed");
        identities++;
    }
    static void state(int actual, int expected) {
        if (actual != expected) throw new AssertionError("original effects changed");
        states++;
    }
    static Object reflected(Method method, Object receiver, long input) throws Throwable {
        try { return method.invoke(receiver, input); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static Method typed, parent, erased;
    static MethodHandle typedHandle, parentHandle;
    static Object shift(int route, Core receiver, long input) throws Throwable {
        switch (route) {
        case 0: return receiver.shift(input);
        case 1: return ((ShiftRoot) receiver).shift(input);
        case 2: return reflected(typed, receiver, input);
        case 3: return reflected(parent, receiver, input);
        case 4: return typedHandle.invoke(receiver, input);
        case 5: return parentHandle.invoke(receiver, input);
        case 6: return Caller.shift(receiver, input);
        default: return reflected(erased, receiver, input);
        }
    }
    interface Run { void call() throws Throwable; }
    static void failure(Run run, Class<?> type, Throwable original) throws Throwable {
        try { run.call(); throw new AssertionError("missing failure"); }
        catch (Throwable error) {
            if (error.getClass() != type) throw new AssertionError("failure type changed", error);
            if (original != null) identity(error, original);
            failures++;
        }
    }
    public static void main(String[] args) throws Throwable {
        typed = Core.class.getMethod("shift", long.class);
        parent = ShiftRoot.class.getMethod("shift", long.class);
        for (Method method : Core.class.getDeclaredMethods())
            if (method.getName().equals("shift") && method.getReturnType() == ShiftRoot.class) erased = method;
        if (erased == null) throw new AssertionError("original erased return entry missing");
        typedHandle = MethodHandles.publicLookup().unreflect(typed);
        parentHandle = MethodHandles.publicLookup().unreflect(parent);
        long[] inputs = {0, -1, Long.MIN_VALUE, Long.MAX_VALUE, 0x1234567887654321L, -0x1234567887654321L};
        for (int route = 0; route < 8; route++) {
            Core left = new Core(1), right = new Core(2);
            int calls = 0;
            for (Api output : new Api[]{left, right, null}) {
                left.output = output;
                for (long input : inputs) {
                    Trace.events = 0;
                    identity(shift(route, left, input), output); identity(left.seen, left);
                    value(Trace.wide, input);
                    state(Trace.events, 1); state(left.calls, ++calls); state(right.calls, 0);
                }
            }
            final int selected = route;
            RuntimeException problem = new IllegalStateException("owned covariant helper failure");
            left.failure = problem; Trace.events = 0;
            failure(() -> shift(selected, left, 9), problem.getClass(), problem);
            value(Trace.wide, 9); state(Trace.events, 1); state(left.calls, ++calls);
            Trace.events = 0;
            failure(() -> shift(selected, null, 8), NullPointerException.class, null);
            state(Trace.events, 0);
        }
        if (typed.isBridge() != args[0].equals("original") || typed.isSynthetic()
                || !erased.isBridge() || !erased.isSynthetic() || typed.getReturnType() != Api.class)
            throw new AssertionError("typed/covariant metadata changed beyond original flag");
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-wide-input-and-covariant-reference-identity=retained,typed-bridge-flag=reported");
    }
}
