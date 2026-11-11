import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class AbsentInterfaceBridgeHarness {
    static int values, identities, states, failures;
    static void state(int actual, int expected) {
        if (actual != expected) throw new AssertionError("interface callback order changed");
        states++;
    }
    static int reflected(Method method, Core receiver, Object other) throws Throwable {
        try { return ((Integer) method.invoke(receiver, other)).intValue(); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        boolean original = args[0].equals("original");
        Method inherited = Api.class.getMethod("compareTo", Object.class);
        if (inherited.getDeclaringClass() != (original ? Comparable.class : Api.class))
            throw new AssertionError("reported added declaration has different ownership");
        try {
            Method added = Api.class.getDeclaredMethod("compareTo", Object.class);
            if (original || !added.isBridge() || !added.isSynthetic() || !added.isDefault())
                throw new AssertionError("added default bridge was not reported exactly");
        } catch (NoSuchMethodException missing) {
            if (!original) throw new AssertionError("reported compiler bridge is missing", missing);
        }
        MethodHandle handle = MethodHandles.publicLookup().unreflect(inherited);
        for (int route = 0; route < 2; route++) {
            Core left = new Core(1), right = new Core(2);
            for (int score : new int[]{Integer.MIN_VALUE, -1, 0, 1, Integer.MAX_VALUE}) {
                left.score = score; right.score = 17;
                Trace.events = 0;
                int before = left.calls;
                int actual = route == 0 ? reflected(inherited, left, right) : (int) handle.invoke(left, (Object) right);
                if (actual != score - 17) throw new AssertionError("full integer result changed");
                values++;
                if (left.seen != right || right.seen != left) throw new AssertionError("original aliases changed");
                identities += 2;
                state(Trace.events, 12); state(left.calls, before + 1); state(right.calls, before + 1);
            }
            RuntimeException failure = new IllegalStateException("owned absent interface slot");
            left.failure = failure; Trace.events = 0;
            try {
                int ignored = route == 0 ? reflected(inherited, left, right) : (int) handle.invoke(left, (Object) right);
                throw new AssertionError("missing original failure");
            } catch (IllegalStateException error) {
                if (error != failure) throw new AssertionError("exception identity changed");
                identities++; failures++;
            }
            state(Trace.events, 1);
            left.failure = null; Trace.events = 0;
            try {
                int ignored = route == 0 ? reflected(inherited, left, new Object()) : (int) handle.invoke(left, new Object());
                throw new AssertionError("missing original argument cast");
            } catch (ClassCastException expected) { failures++; }
            state(Trace.events, 0);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("inherited-erased-dispatch=matched,added-interface-default-and-reflection=reported");
    }
}
