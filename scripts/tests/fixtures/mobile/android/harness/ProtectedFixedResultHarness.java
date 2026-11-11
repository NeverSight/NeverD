import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ProtectedFixedResultHarness {
    public static final class Override extends Core<Object> {
        public int overrides;
        protected Slot resolve(Token token) { overrides++; return super.resolve(token); }
    }
    static int values, identities, states, failures;
    static Method typed, erased;
    static MethodHandle handle;
    static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static Object call(int route, Core<?> self, Token token, int index, long wide, double fraction) throws Throwable {
        try {
            switch (route) {
            case 0: return self.typed(token, index, wide, fraction);
            case 1: return ((Base<Slot>)self).dispatch(token, index, wide, fraction);
            case 2: return erased.invoke(self, token, index, wide, fraction);
            case 3: return handle.invoke(self, token, index, wide, fraction);
            case 4: return ProtectedFixedResultCaller.call(self, token, index, wide, fraction);
            default: return typed.invoke(self, token, index, wide, fraction);
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args[0].equals("rebuilt");
        for (Method method : Core.class.getDeclaredMethods()) {
            if (!method.getName().equals("get")) continue;
            if (method.getReturnType() == Slot.class) typed = method;
            if (method.getReturnType() == Object.class) erased = method;
        }
        if (typed == null || erased == null || !Modifier.isAbstract(Core.class.getModifiers())
                || !Modifier.isFinal(typed.getModifiers())
                || !(rebuilt ? Modifier.isPublic(typed.getModifiers()) : Modifier.isProtected(typed.getModifiers()))
                || !Modifier.isPublic(erased.getModifiers()) || !erased.isBridge() || !erased.isSynthetic()
                || !Modifier.isProtected(Override.class.getDeclaredMethod("resolve", Token.class).getModifiers()))
            throw new AssertionError("final target or immutable override metadata");
        typed.setAccessible(true); handle = MethodHandles.publicLookup().unreflect(erased);
        long[] longs = {Long.MIN_VALUE, Long.MAX_VALUE, 0, -1};
        double[] doubles = {-0.0, Double.longBitsToDouble(0x7ff8000000000042L), Double.POSITIVE_INFINITY, Double.MIN_VALUE};
        for (int route = 0; route < 6; route++)
            for (boolean override : new boolean[]{false, true})
                for (int at = 0; at < 4; at++)
                    for (boolean nilInput : new boolean[]{false, true})
                        for (boolean nilResult : new boolean[]{false, true})
                            for (boolean throwing : new boolean[]{false, true}) {
                                Core<?> self = override ? new Override() : new Child<Object>();
                                Token token = nilInput ? null : new Token();
                                Slot expected = nilResult ? null : new Slot();
                                if (token != null) token.value = expected;
                                RuntimeException marker = new IllegalStateException("owned fixed result failure");
                                self.failure = throwing ? marker : null;
                                Object actual = null; Throwable error = null;
                                int index = Integer.MIN_VALUE + at;
                                try { actual = call(route, self, token, index, longs[at], doubles[at]); }
                                catch (Throwable problem) { error = problem; }
                                state(self.calls == 1 && self.visits == (route == 1 || route == 4 ? 1 : 0));
                                value(self.index == index && self.wide == longs[at] && self.bits == Double.doubleToRawLongBits(doubles[at]));
                                if (nilInput) { value(error instanceof NullPointerException); failures++; }
                                else {
                                    state(token.reads == 1); same(token.receiver, self);
                                    if (throwing) { same(error, marker); failures++; }
                                    else { same(error, null); same(actual, expected); }
                                }
                                int called = nilInput || throwing ? 0 : 1;
                                state(self.resolves == called && (!override || ((Override)self).overrides == called));
                                same(self.last, called == 1 ? expected : null);
                            }
        for (int route = 0; route < 6; route++) {
            try { call(route, null, null, 0, 0L, 0.0); throw new AssertionError("null receiver"); }
            catch (NullPointerException expected) { failures++; }
        }
        System.out.println("values="+values+",identities="+identities+",states="+states+",failures="+failures);
        System.out.println("fixed-arguments-and-final-result-slot=retained,immutable-callback-and-failure-order=matched");
    }
}
