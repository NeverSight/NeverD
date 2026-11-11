import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class InheritedCheckedResultHarness {
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
    static final class Derived extends Core {
        int entries;
        public Value fetch() { entries++; return super.fetch(); }
        public Value fetch(int index) { entries++; return super.fetch(index); }
        public Value fetch(long stamp, double fraction, Object token) {
            entries++; return super.fetch(stamp, fraction, token);
        }
    }
    static Class<?>[] parameters(int shape) {
        return shape == 0 ? new Class<?>[]{} : shape == 1 ? new Class<?>[]{int.class}
            : new Class<?>[]{long.class, double.class, Object.class};
    }
    static Value call(int route, int shape, Core self, Object token) throws Throwable {
        int index = Integer.MIN_VALUE;
        long stamp = Long.MIN_VALUE + 17;
        double fraction = Double.longBitsToDouble(0xfff0000000000001L);
        Object[] args = shape == 0 ? new Object[]{} : shape == 1 ? new Object[]{index}
            : new Object[]{stamp, fraction, token};
        try {
            switch (route) {
            case 0: return shape == 0 ? self.fetch() : shape == 1 ? self.fetch(index) : self.fetch(stamp, fraction, token);
            case 1: return shape == 0 ? ((Api)self).fetch() : shape == 1 ? ((Api)self).fetch(index) : ((Api)self).fetch(stamp, fraction, token);
            case 2: return (Value)Core.class.getDeclaredMethod("fetch", parameters(shape)).invoke(self, args);
            case 3:
                MethodHandle handle = MethodHandles.publicLookup().findVirtual(Core.class, "fetch",
                    MethodType.methodType(Value.class, parameters(shape)));
                return shape == 0 ? (Value)handle.invokeExact(self) : shape == 1 ? (Value)handle.invokeExact(self, index)
                    : (Value)handle.invokeExact(self, stamp, fraction, token);
            case 4: return shape == 0 ? InheritedCheckedResultCaller.zero(self) : shape == 1 ? InheritedCheckedResultCaller.indexed(self, index)
                    : InheritedCheckedResultCaller.wide(self, stamp, fraction, token);
            default: return shape == 0 ? Caller.zero(self) : shape == 1 ? Caller.indexed(self, index)
                    : Caller.wide(self, stamp, fraction, token);
            }
        } catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    static Object raw(int route, Core self) throws Throwable {
        try {
            switch (route) {
            case 0: return ((Parent)self).fetch(19);
            case 1: return Parent.class.getMethod("fetch", int.class).invoke(self, 19);
            case 2: return (Object)MethodHandles.publicLookup().findVirtual(Parent.class, "fetch",
                MethodType.methodType(Object.class, int.class)).invokeExact((Parent)self, 19);
            case 3: return InheritedCheckedResultCaller.raw(self, 19);
            default: return Caller.raw(self, 19);
            }
        } catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        for (Class<?> owner : new Class<?>[]{Core.class, Host.Nested.class}) {
            ParameterizedType parent = (ParameterizedType)owner.getGenericSuperclass();
            same(parent.getRawType(), Parent.class); same(parent.getActualTypeArguments()[0], Value.class);
            int original = 0;
            for (Method method : owner.getDeclaredMethods()) {
                if (method.getName().startsWith("__neverd")) continue;
                value(method.getName().equals("fetch") && method.isBridge() && method.isSynthetic()
                    && method.getReturnType() == Value.class);
                original++;
            }
            value(original == 3);
        }
        for (int route = 0; route < 6; route++) for (int shape = 0; shape < 3; shape++)
            for (boolean derived : new boolean[]{false, true}) {
                final int r = route, s = shape;
                Core self = derived ? new Derived() : new Core();
                Object token = new Object(); Value first = new Value(); first.marker = 9;
                int count = 0;
                for (Value item : new Value[]{null, first, new Value()}) {
                    self.stored = item; same(call(route, shape, self, token), item); count++;
                    state(self.calls == count);
                    if (shape == 0) value(self.index == -1);
                    if (shape == 1) value(self.index == Integer.MIN_VALUE);
                    if (shape == 2) {
                        value(self.stamp == Long.MIN_VALUE + 17);
                        value(Double.doubleToRawLongBits(self.fraction) == 0xfff0000000000001L);
                        same(self.seen, token);
                    }
                    if (derived) state(((Derived)self).entries == count);
                }
                self.stored = new Object();
                failure(() -> call(r, s, self, token), ClassCastException.class, null); count++;
                state(self.calls == count);
                RuntimeException original = new IllegalStateException("owned failure before result cast");
                self.failure = original;
                failure(() -> call(r, s, self, token), original.getClass(), original); count++;
                state(self.calls == count);
                if (derived) state(((Derived)self).entries == count);
                failure(() -> call(r, s, null, token), NullPointerException.class, null);
                self.failure = null; self.stored = first; first.marker = 27;
                same(call(r, s, self, token), first); value(first.marker == 27);
            }
        for (int route = 0; route < 5; route++) {
            final int r = route;
            Core self = new Core(); Object incompatible = new Object(); self.stored = incompatible;
            same(raw(route, self), incompatible); state(self.calls == 1 && self.index == 19);
            Derived derived = new Derived(); derived.stored = incompatible;
            failure(() -> raw(r, derived), ClassCastException.class, null);
            state(derived.calls == 1 && derived.entries == 1);
        }
        Host.Nested nested = new Host.Nested(); Value item = new Value(); nested.stored = item;
        same(Caller.nested(nested, 3), item); same(InheritedCheckedResultCaller.nested(nested, 4), item);
        nested.stored = new Object();
        failure(() -> Caller.nested(nested, 5), ClassCastException.class, null);
        same(((Parent)nested).fetch(6), nested.stored); state(nested.calls == 4 && nested.index == 6);
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-narrow-bridges-and-inherited-broad-entry=retained,heap-pollution-and-external-overrides=matched,wide-arguments-and-ordered-failures=matched");
    }
}
