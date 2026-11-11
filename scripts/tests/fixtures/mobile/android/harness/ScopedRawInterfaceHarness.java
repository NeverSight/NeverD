import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ScopedRawInterfaceHarness {
    static int values, identities, states, failures;
    static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    interface Action { void run() throws Throwable; }
    static void failure(Action action, Class<?> kind, Throwable original) throws Throwable {
        try { action.run(); throw new AssertionError("missing failure"); }
        catch (Throwable problem) {
            if (problem.getClass() != kind) throw new AssertionError("wrong failure", problem);
            if (original != null) same(problem, original);
            failures++;
        }
    }
    static final class Override extends Core<Leaf> {
        int entries;
        final Object marker = new Object();
        public boolean matches(Bound item) { entries++; return !super.matches(item); }
        public Object copy() { return marker; }
    }
    static boolean match(int route, Core self, Object item) throws Throwable {
        try {
            switch (route) {
            case 0: return self.matches((Bound)item);
            case 1: return ((Root)self).matches(item);
            case 2: return ((Api)self).matches(item);
            case 3: return (Boolean)Core.class.getMethod("matches", Object.class).invoke(self, item);
            case 4: return (boolean)MethodHandles.publicLookup().findVirtual(Core.class, "matches",
                MethodType.methodType(boolean.class, Object.class)).invokeExact(self, item);
            case 5: return ScopedRawInterfaceCaller.erased(self, item);
            default: return Caller.erased(self, item);
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        TypeVariable<?> formal = Core.class.getTypeParameters()[0];
        value(formal.getBounds().length == 1 && formal.getBounds()[0] == Bound.class);
        ParameterizedType parent = (ParameterizedType)Core.class.getGenericInterfaces()[0];
        same(parent.getRawType(), Api.class); same(parent.getActualTypeArguments()[0], formal);
        Method typed = Core.class.getMethod("matches", Bound.class);
        value(typed.getGenericParameterTypes()[0] == Bound.class && !typed.isBridge() && !typed.isSynthetic());
        Method bridge = Core.class.getMethod("matches", Object.class);
        value(bridge.isBridge() && bridge.isSynthetic() && bridge.getReturnType() == boolean.class);
        for (int route = 0; route < 7; route++) {
            final int selected = route;
            for (boolean overridden : new boolean[]{false, true}) {
                Core self = overridden ? new Override() : new Core(); self.nullValue = true;
                Bound first = new Bound(); first.accepted = true;
                Bound second = new Bound(); second.accepted = false;
                Bound leaf = new Leaf(); leaf.accepted = true;
                int count = 0;
                for (Bound item : new Bound[]{null, first, second, leaf}) {
                    boolean wanted = item == null || item.accepted;
                    value(match(route, self, item) == (wanted ^ overridden));
                    count++;
                    state(self.calls == count && (item == null || item.reads == 1));
                    same(self.observed, item);
                    if (overridden) state(((Override)self).entries == count);
                }
                Object copied = overridden ? ((Override)self).marker : self;
                same(((Root)self).copy(), copied); same(ScopedRawInterfaceCaller.copy(self), copied);
                same(Caller.copy(self), copied);
                RuntimeException early = new IllegalArgumentException("owned early failure");
                RuntimeException late = new IllegalStateException("owned late failure");
                self.failure = early; first.failure = late;
                failure(() -> match(selected, self, first), early.getClass(), early);
                state(self.calls == 5 && first.reads == 1); same(self.observed, first);
                self.failure = null;
                failure(() -> match(selected, self, first), late.getClass(), late);
                state(self.calls == 6 && first.reads == 2); same(self.observed, first);
                failure(() -> match(selected, self, new Object()), ClassCastException.class, null);
                state(self.calls == 6 && first.reads == 2);
                failure(() -> match(selected, null, first), NullPointerException.class, null);
                state(self.calls == 6 && first.reads == 2);
                first.failure = null; first.accepted = false;
                value(Caller.typed(self, first) == overridden);
                state(self.calls == 7 && first.reads == 3);
                if (overridden) state(((Override)self).entries == 7);
            }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-interface-diamond-and-class-scope=retained,raw-target-and-bridge-metadata=retained,external-overrides-and-ordered-effects=retained");
    }
}
