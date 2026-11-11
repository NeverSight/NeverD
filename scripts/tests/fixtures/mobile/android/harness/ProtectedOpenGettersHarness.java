import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

public final class ProtectedOpenGettersHarness {
    public static final class Override extends Core<Object> {
        public int overrides;
        protected Slot get() { overrides++; return super.get(); }
    }
    private static int values, identities, states, failures;
    private static Method erased, typed;
    private static MethodHandle handle;
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("value"); values++;
    }
    private static void state(int actual, int expected) {
        if (actual != expected) throw new AssertionError("state: " + actual + ":" + expected); states++;
    }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("identity"); identities++;
    }
    private static int overrides(Core<?> receiver) {
        return receiver instanceof Override ? ((Override) receiver).overrides : 0;
    }
    private static Slot invoke(int route, Core<?> receiver) throws Throwable {
        try {
            switch (route) {
                case 0: return receiver.typed();
                case 1: return ((Base<Slot>) receiver).dispatch();
                case 2: return (Slot) erased.invoke(receiver);
                case 3: return (Slot) handle.invoke(receiver);
                case 4: return (Slot) ProtectedGetterCaller.call(receiver);
                case 5: return (Slot) typed.invoke(receiver);
                default: throw new AssertionError("route");
            }
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args[0].equals("rebuilt");
        for (Method method : Core.class.getDeclaredMethods()) {
            if (!method.getName().equals("get")) continue;
            if (method.getReturnType() == Broad.class) erased = method;
            if (method.getReturnType() == Slot.class) typed = method;
        }
        if (erased == null || typed == null || !erased.isBridge() || !erased.isSynthetic()
                || !Modifier.isPublic(erased.getModifiers()) || Modifier.isFinal(Core.class.getModifiers())
                || Core.class.getTypeParameters().length != 1
                || (rebuilt ? !Modifier.isPublic(typed.getModifiers()) : !Modifier.isProtected(typed.getModifiers())))
            throw new AssertionError("original hierarchy or reported visibility");
        if (!Modifier.isProtected(Override.class.getDeclaredMethod("get").getModifiers()))
            throw new AssertionError("immutable protected override changed");
        typed.setAccessible(true); handle = MethodHandles.publicLookup().unreflect(erased);
        Core<?>[] receivers = {new Core<Object>(), new Child<Object>(), new Override()};
        Slot[] inputs = {null, new Slot(-1), new Slot(0)};
        for (Core<?> receiver : receivers) for (int route = 0; route < 6; route++)
            for (Slot input : inputs) for (boolean allocate : new boolean[]{false, true})
                for (boolean throwing : new boolean[]{false, true}) {
                    receiver.value = input; receiver.allocate = allocate; receiver.marker = 17;
                    RuntimeException problem = throwing ? new IllegalStateException("owned-getter-failure") : null;
                    receiver.failure = problem;
                    int beforeCalls = receiver.calls, beforeVisits = receiver.visits, beforeOverrides = overrides(receiver);
                    int beforeCreated = Slot.created; Slot beforeLast = receiver.last;
                    try {
                        Slot result = invoke(route, receiver);
                        if (throwing) throw new AssertionError("failure missing");
                        identity(result, receiver.value);
                        if (allocate && result == input) throw new AssertionError("fresh allocation missing");
                        if (result != null) {
                            value(result.value, allocate ? 17 : input.value);
                            result.value += 5; state(receiver.value.value, result.value);
                        }
                    } catch (RuntimeException error) {
                        if (!throwing) throw error;
                        identity(error, problem); failures++;
                    }
                    state(receiver.calls, beforeCalls + 1);
                    state(receiver.visits, beforeVisits + (route == 1 || route == 4 ? 1 : 0));
                    state(overrides(receiver), beforeOverrides + (receiver instanceof Override ? 1 : 0));
                    state(Slot.created, beforeCreated + (allocate && !throwing ? 1 : 0));
                    identity(receiver.last, throwing ? beforeLast : receiver.value);
                }
        for (int route = 0; route < 6; route++) {
            try { invoke(route, null); throw new AssertionError("null receiver accepted"); }
            catch (NullPointerException expected) { failures++; }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("public-erased-getter-and-protected-external-override=retained,typed-access-change=reported");
    }
}
