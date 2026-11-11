import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

public final class ProtectedArrayResultsHarness {
    public static final class Override extends Core<Object> {
        public int overrides;
        protected Slot[] allocate(int length) { overrides++; return super.allocate(length); }
    }
    private static int values, identities, states, failures;
    private static Method erased, typed;
    private static MethodHandle handle;
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("value: " + actual + ":" + expected); values++;
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
    private static Slot[] invoke(int route, Core<?> receiver, int length) throws Throwable {
        try {
            switch (route) {
                case 0: return receiver.typed(length);
                case 1: return ((Base<Slot>) receiver).dispatch(length);
                case 2: return (Slot[]) erased.invoke(receiver, length);
                case 3: return (Slot[]) handle.invoke(receiver, length);
                case 4: return (Slot[]) ProtectedArrayCaller.call(receiver, length);
                case 5: return (Slot[]) typed.invoke(receiver, length);
                default: throw new AssertionError("route");
            }
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args[0].equals("rebuilt");
        for (Method method : Core.class.getDeclaredMethods()) {
            if (!method.getName().equals("allocate")) continue;
            if (method.getReturnType() == Broad[].class) erased = method;
            if (method.getReturnType() == Slot[].class) typed = method;
        }
        if (erased == null || typed == null || !erased.isBridge() || !erased.isSynthetic()
                || !Modifier.isPublic(erased.getModifiers()) || Modifier.isFinal(Core.class.getModifiers())
                || Core.class.getTypeParameters().length != 1
                || (rebuilt ? !Modifier.isPublic(typed.getModifiers()) : !Modifier.isProtected(typed.getModifiers())))
            throw new AssertionError("original hierarchy or reported visibility");
        if (!Modifier.isProtected(Override.class.getDeclaredMethod("allocate", int.class).getModifiers()))
            throw new AssertionError("immutable protected override changed");
        typed.setAccessible(true); handle = MethodHandles.publicLookup().unreflect(erased);
        Core<?>[] receivers = {new Core<Object>(), new Child<Object>(), new Override()};
        Slot[][] inputs = {null, new Slot[0], new Slot[]{new Slot(11), new Slot(13)}};
        for (Core<?> receiver : receivers) for (int route = 0; route < 6; route++)
            for (Slot[] input : inputs) for (boolean cached : new boolean[]{false, true})
                for (int length : new int[]{-1, 0, 1, 3})
                    for (boolean throwing : new boolean[]{false, true}) {
                        receiver.value = input; receiver.cached = cached;
                        RuntimeException problem = throwing ? new IllegalStateException("owned-array-failure") : null;
                        receiver.failure = problem;
                        int beforeCalls = receiver.calls, beforeVisits = receiver.visits, beforeOverrides = overrides(receiver);
                        Slot[] beforeLast = receiver.last;
                        boolean shouldFail = throwing || !cached && length < 0;
                        try {
                            Slot[] result = invoke(route, receiver, length);
                            if (shouldFail) throw new AssertionError("failure missing");
                            identity(result, receiver.value); identity(receiver.last, result);
                            identity(Core.view(result), result);
                            if (!cached && result == input) throw new AssertionError("fresh allocation missing");
                            if (result != null) {
                                identity(result.getClass(), Slot[].class);
                                value(result.length, cached ? input.length : length);
                                if (!cached) for (Slot element : result)
                                    if (element != null) throw new AssertionError("array initialization");
                                if (result.length > 0) {
                                    Slot item = new Slot(37); result[0] = item;
                                    identity(receiver.value[0], item); item.value += 3;
                                    value(result[0].value, 40);
                                    Broad<?>[] erasedArray = result;
                                    try { erasedArray[0] = new Broad<String>(); throw new AssertionError("array store accepted"); }
                                    catch (ArrayStoreException expected) { failures++; }
                                    identity(result[0], item);
                                }
                            }
                        } catch (RuntimeException error) {
                            if (!shouldFail) throw error;
                            if (throwing) identity(error, problem);
                            else if (!(error instanceof NegativeArraySizeException)) throw error;
                            failures++;
                        }
                        state(receiver.calls, beforeCalls + 1); state(receiver.requested, length);
                        state(receiver.visits, beforeVisits + (route == 1 || route == 4 ? 1 : 0));
                        state(overrides(receiver), beforeOverrides + (receiver instanceof Override ? 1 : 0));
                        if (shouldFail) {
                            identity(receiver.last, beforeLast); identity(receiver.value, input);
                        }
                    }
        for (int route = 0; route < 6; route++) {
            try { invoke(route, null, 1); throw new AssertionError("null receiver accepted"); }
            catch (NullPointerException expected) { failures++; }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("public-erased-array-result-and-protected-external-override=retained,typed-access-change=reported");
    }
}
