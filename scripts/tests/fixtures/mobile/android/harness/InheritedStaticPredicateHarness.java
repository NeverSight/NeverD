import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class InheritedStaticPredicateHarness {
    static int values, identities, states, failures;
    static void value(boolean actual, boolean expected) {
        if (actual != expected) throw new AssertionError("boolean result changed");
        values++;
    }
    static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original receiver alias changed");
        identities++;
    }
    static void state(int actual, int expected) {
        if (actual != expected) throw new AssertionError("original predicate effects changed");
        states++;
    }
    static boolean reflected(Method method, Object receiver) throws Throwable {
        try { return ((Boolean) method.invoke(receiver)).booleanValue(); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static Method[] typed = new Method[2], parent = new Method[2], inherited = new Method[2];
    static MethodHandle[] typedHandles = new MethodHandle[2], parentHandles = new MethodHandle[2];
    static boolean test(int route, int which, Core receiver) throws Throwable {
        switch (route) {
        case 0: return which == 0 ? receiver.ready() : receiver.notReady();
        case 1: return which == 0 ? ((PredicateRoot) receiver).ready() : ((PredicateRoot) receiver).notReady();
        case 2: return reflected(typed[which], receiver);
        case 3: return reflected(parent[which], receiver);
        case 4: return (boolean) typedHandles[which].invoke(receiver);
        case 5: return (boolean) parentHandles[which].invoke(receiver);
        case 6: return Caller.ready(receiver, which != 0);
        default: return reflected(inherited[which], receiver);
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
        int index = 0;
        for (String name : new String[]{"ready", "notReady"}) {
            typed[index] = Core.class.getMethod(name);
            parent[index] = PredicateRoot.class.getMethod(name);
            inherited[index] = Api.class.getMethod(name);
            if (inherited[index].getDeclaringClass() != PredicateRoot.class)
                throw new AssertionError("inherited predicate was redeclared");
            typedHandles[index] = MethodHandles.publicLookup().unreflect(typed[index]);
            parentHandles[index] = MethodHandles.publicLookup().unreflect(parent[index]);
            if (typed[index].isBridge() != args[0].equals("original") || typed[index].isSynthetic())
                throw new AssertionError("typed predicate flag differs from report");
            index++;
        }
        for (int route = 0; route < 8; route++) {
            Core receiver = new Core(1); int calls = 0;
            for (int score : new int[]{Integer.MIN_VALUE, -1, 0, 1, Integer.MAX_VALUE}) {
                receiver.score = score;
                for (int which = 0; which < 2; which++) {
                    Trace.events = 0;
                    value(test(route, which, receiver), which == 0 ? score >= 0 : score < 0);
                    identity(receiver.seen, receiver);
                    state(Trace.events, 1); state(receiver.calls, ++calls); state(receiver.score, score);
                }
            }
            RuntimeException problem = new IllegalStateException("owned inherited predicate failure");
            receiver.failure = problem;
            for (int which = 0; which < 2; which++) {
                final int selected = route, predicate = which;
                Trace.events = 0;
                failure(() -> test(selected, predicate, receiver), problem.getClass(), problem);
                state(Trace.events, 1); state(receiver.calls, ++calls);
                Trace.events = 0;
                failure(() -> test(selected, predicate, null), NullPointerException.class, null);
                state(Trace.events, 0);
            }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("inherited-predicate-declarations-original-helpers-and-receiver-effects=retained,typed-bridge-flags=reported");
    }
}
