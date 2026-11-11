import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class IntegerStaticComparisonHarness {
    static int values, identities, states, failures;
    static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("integer result bits changed");
        values++;
    }
    static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original alias changed");
        identities++;
    }
    static void state(int actual, int expected) {
        if (actual != expected) throw new AssertionError("original effect order changed");
        states++;
    }
    static int reflected(Method method, Object receiver, Object other) throws Throwable {
        try { return ((Integer) method.invoke(receiver, other)).intValue(); }
        catch (InvocationTargetException e) { throw e.getCause(); }
    }
    static final Method typed, erased, parent;
    static final MethodHandle typedHandle, erasedHandle;
    static {
        try {
            typed = Core.class.getMethod("compareTo", Api.class);
            erased = Core.class.getMethod("compareTo", Object.class);
            parent = Comparable.class.getMethod("compareTo", Object.class);
            typedHandle = MethodHandles.publicLookup().unreflect(typed);
            erasedHandle = MethodHandles.publicLookup().unreflect(erased);
        } catch (Exception e) { throw new AssertionError(e); }
    }
    static int compare(int route, Core receiver, Object other) throws Throwable {
        switch (route) {
        case 0: return Caller.typed(receiver, (Api) other);
        case 1: return Caller.erased(receiver, other);
        case 2: return reflected(typed, receiver, other);
        case 3: return reflected(erased, receiver, other);
        case 4: return reflected(parent, receiver, other);
        case 5: return (int) typedHandle.invoke(receiver, (Api) other);
        case 6: return (int) erasedHandle.invoke(receiver, other);
        default: return ((Comparable) receiver).compareTo(other);
        }
    }
    interface Run { void call() throws Throwable; }
    static void failure(Run run, Class<?> type, Throwable original, int events) throws Throwable {
        Trace.events = 0;
        try { run.call(); throw new AssertionError("missing failure"); }
        catch (Throwable e) {
            if (e.getClass() != type) throw new AssertionError("exception type changed", e);
            if (original != null) identity(e, original);
            failures++;
        }
        state(Trace.events, events);
    }
    public static void main(String[] args) throws Throwable {
        boolean original = args[0].equals("original");
        int[][] pairs = {{0,0},{1,-1},{Integer.MIN_VALUE,1},{Integer.MAX_VALUE,-1},{-2147483647,2147483646}};
        for (int route = 0; route < 8; route++) {
            Core left = new Core(1), right = new Core(2);
            for (int[] pair : pairs) {
                left.score = pair[0]; right.score = pair[1];
                int beforeLeft = left.calls, beforeRight = right.calls;
                Trace.events = 0;
                value(compare(route, left, right), pair[0] - pair[1]);
                state(Trace.events, 12); state(left.calls, beforeLeft + 1); state(right.calls, beforeRight + 1);
                identity(left.seen, right); identity(right.seen, left);
                Trace.events = 0; beforeLeft = left.calls;
                value(compare(route, left, left), 0);
                state(Trace.events, 11); state(left.calls, beforeLeft + 2); identity(left.seen, left);
            }
            final int selected = route;
            RuntimeException problem = new IllegalStateException("owned integer default failure");
            left.failure = problem;
            failure(() -> compare(selected, left, right), problem.getClass(), problem, 1);
            left.failure = null; right.failure = problem;
            failure(() -> compare(selected, left, right), problem.getClass(), problem, 12);
            right.failure = null;
            failure(() -> compare(selected, left, null), NullPointerException.class, null, 1);
            identity(left.seen, null);
            failure(() -> compare(selected, null, right), NullPointerException.class, null, 0);
            if (route == 1 || route == 3 || route == 4 || route == 6 || route == 7)
                failure(() -> compare(selected, left, new Object()), ClassCastException.class, null, 0);
        }
        if (typed.isBridge() != original || typed.isSynthetic() || !erased.isBridge() || !erased.isSynthetic())
            throw new AssertionError("original typed and erased bridge flags changed beyond report");
        Type edge = Api.class.getGenericInterfaces()[0];
        if (((ParameterizedType) edge).getActualTypeArguments()[0] != Api.class || Core.class.getTypeParameters().length != 0)
            throw new AssertionError("original recursive interface contract changed");
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-static-helper-arguments-callbacks-and-integer-results=retained,typed-bridge-flag=reported");
    }
}
