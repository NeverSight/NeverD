import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings("unchecked")
public final class ErasedResultInvocationHarness {
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
    static final class Derived extends Provider {
        int entries;
        public <T extends Value> T read(Object token, int index, long stamp, double fraction) {
            entries++; return super.read(token, index, stamp, fraction);
        }
    }
    static final class Other implements Value {}
    static final long WORD = Long.MIN_VALUE + 27;
    static final long NAN = 0xfff0000000000001L;
    static final int INDEX = Integer.MIN_VALUE + 19;
    static final double BITS = Double.longBitsToDouble(NAN);
    static final Class<?>[] PARAMS = {Object.class, int.class, long.class, double.class};
    static Value call(int route, Provider self, Object token) throws Throwable {
        try {
            switch (route) {
            case 0: return self.read(token, INDEX, WORD, BITS);
            case 1: return ((Api)self).read(token, INDEX, WORD, BITS);
            case 2: return (Value)Provider.class.getMethod("read", PARAMS).invoke(self, token, INDEX, WORD, BITS);
            case 3: return (Value)MethodHandles.publicLookup().findVirtual(Provider.class, "read",
                MethodType.methodType(Value.class, PARAMS)).invokeExact(self, token, INDEX, WORD, BITS);
            case 4: return ErasedResultInvocationCaller.direct(self, token, INDEX, WORD, BITS);
            case 5: return Caller.direct(self, token, INDEX, WORD, BITS);
            default: return Caller.viaApi(self, token, INDEX, WORD, BITS);
            }
        } catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    static Item checked(int route, Provider self, Object token) {
        switch (route) {
        case 0: return self.read(token, INDEX, WORD, BITS);
        case 1: return ErasedResultInvocationCaller.checked(self, token, INDEX, WORD, BITS);
        default: return Caller.checked(self, token, INDEX, WORD, BITS);
        }
    }
    static Box stat(int route, Object token) throws Throwable {
        try {
            switch (route) {
            case 0: return Provider.staticRead(token, INDEX, WORD, BITS);
            case 1: return ErasedResultInvocationCaller.stat(token, INDEX, WORD, BITS);
            case 2: return Caller.stat(token, INDEX, WORD, BITS);
            case 3: return (Box)Provider.class.getMethod("staticRead", PARAMS).invoke(null, token, INDEX, WORD, BITS);
            default: return (Box)MethodHandles.publicLookup().findStatic(Provider.class, "staticRead",
                MethodType.methodType(Box.class, PARAMS)).invokeExact(token, INDEX, WORD, BITS);
            }
        } catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    static void recorded(Provider self, Object token, int count) {
        state(self.calls == count); same(self.seen, token);
        value(self.index == INDEX && self.stamp == WORD && Double.doubleToRawLongBits(self.fraction) == NAN);
        if (self instanceof Derived) state(((Derived)self).entries == count);
    }
    public static void main(String[] args) throws Throwable {
        for (Class<?> owner : new Class<?>[]{Api.class, Provider.class}) {
            Method method = owner.getMethod("read", PARAMS);
            TypeVariable<?>[] formals = method.getTypeParameters();
            value(formals.length == 1); same(formals[0].getBounds()[0], Value.class);
            value(formals[0].getGenericDeclaration().equals(method));
            value(method.getGenericReturnType().equals(formals[0]));
            same(method.getReturnType(), Value.class); value(method.getExceptionTypes().length == 0);
            for (Type type : method.getGenericParameterTypes()) value(type instanceof Class<?>);
            value(!method.isBridge() && !method.isSynthetic());
        }
        Method staticMethod = Provider.class.getMethod("staticRead", PARAMS);
        same(staticMethod.getTypeParameters()[0].getBounds()[0], Box.class);
        value(staticMethod.getGenericReturnType().equals(staticMethod.getTypeParameters()[0]));
        for (int route = 0; route < 7; route++) for (boolean derived : new boolean[]{false, true}) {
            final int r = route; Provider self = derived ? new Derived() : new Provider();
            Object token = new Object(); Item item = new Item(); item.marker = 7; int count = 0;
            for (Value stored : new Value[]{null, item, new Other()}) {
                self.stored = stored; same(call(r, self, token), stored); recorded(self, token, ++count);
            }
            self.stored = new Object();
            failure(() -> call(r, self, token), ClassCastException.class, null); recorded(self, token, ++count);
            RuntimeException original = new IllegalStateException("owned before result cast"); self.failure = original;
            failure(() -> call(r, self, token), original.getClass(), original); recorded(self, token, ++count);
            failure(() -> call(r, null, token), NullPointerException.class, null); recorded(self, token, count);
            self.failure = null; self.stored = item; item.marker = 29;
            same(call(r, self, token), item); recorded(self, token, ++count); value(item.marker == 29);
        }
        for (int route = 0; route < 3; route++) {
            final int r = route; Provider self = new Derived(); Object token = new Object(); Item item = new Item();
            self.stored = item; same(checked(r, self, token), item); recorded(self, token, 1);
            self.stored = new Other(); failure(() -> checked(r, self, token), ClassCastException.class, null); recorded(self, token, 2);
            RuntimeException original = new UnsupportedOperationException("owned checked call"); self.failure = original;
            failure(() -> checked(r, self, token), original.getClass(), original); recorded(self, token, 3);
            failure(() -> checked(r, null, token), NullPointerException.class, null); recorded(self, token, 3);
        }
        Provider ignored = new Derived(); Object token = new Object();
        ignored.stored = new Other(); Caller.ignored(ignored, token, INDEX, WORD, BITS); recorded(ignored, token, 1);
        ignored.stored = new Object();
        failure(() -> Caller.ignored(ignored, token, INDEX, WORD, BITS), ClassCastException.class, null); recorded(ignored, token, 2);
        for (int route = 0; route < 5; route++) {
            final int r = route; int count = 0; Provider.staticCalls = 0; Provider.staticFailure = null;
            Box box = new Box(); box.marker = 31;
            for (Box stored : new Box[]{null, box, new Box()}) {
                Provider.staticStored = stored; same(stat(r, token), stored);
                state(Provider.staticCalls == ++count); same(Provider.staticSeen, token);
                value(Provider.staticIndex == INDEX && Provider.staticStamp == WORD && Double.doubleToRawLongBits(Provider.staticFraction) == NAN);
            }
            Provider.staticStored = new Object(); failure(() -> stat(r, token), ClassCastException.class, null); state(Provider.staticCalls == ++count);
            RuntimeException original = new IllegalArgumentException("owned static failure"); Provider.staticFailure = original;
            failure(() -> stat(r, token), original.getClass(), original); state(Provider.staticCalls == ++count);
            Provider.staticFailure = null; Provider.staticStored = box; same(stat(r, token), box); value(box.marker == 31);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("result-only-formals-and-original-descriptors=retained,checked-results-and-provider-failures=ordered,external-overrides-and-wide-inputs=matched");
    }
}
