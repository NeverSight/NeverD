import fixture.Api;
import fixture.Parent;
import fixture.Core;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

public final class RawMethodFormalMembersHarness {
    private static int values, identities, states, failures;
    private static final Method CORE, ROOT, DEFAULT;
    private static final MethodHandle HANDLE;
    static {
        try {
            CORE = Core.class.getDeclaredMethod("array", Object[].class, Object.class);
            ROOT = Parent.class.getMethod("array", Object[].class, Object.class);
            DEFAULT = Parent.class.getMethod("call", Object[].class, Object.class);
            HANDLE = MethodHandles.lookup().findVirtual(Parent.class, "array", MethodType.methodType(Object[].class, Object[].class, Object.class));
        } catch (Exception error) { throw new AssertionError(error); }
    }
    private static void same(Object expected, Object actual) {
        if (expected != actual) throw new AssertionError("identity changed");
        identities++;
    }
    private static void state(int expected, int actual) {
        if (expected != actual) throw new AssertionError("state changed");
        states++;
    }
    private static Object[] call(int route, Core core, Object[] input, Object tag) throws Throwable {
        try {
            switch (route) {
                case 0: return core.array(input, tag);
                case 1: return ((Api) core).array(input, tag);
                case 2: return ((Parent) core).array(input, tag);
                case 3: return (Object[]) CORE.invoke(core, input, tag);
                case 4: return (Object[]) ROOT.invoke(core, input, tag);
                case 5: return (Object[]) HANDLE.invokeExact((Parent) core, input, tag);
                case 6: return ((Api) core).call(input, tag);
                case 8: return core.entry(input, tag);
                case 7: return (Object[]) DEFAULT.invoke(core, input, tag);
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void check(Core core, Object[] input, int[] tag, int count, int marker) {
        state(count, core.calls); state(marker, core.marker); state(count, core.seen.size());
        same(tag, core.observed); same(input, core.lastInput); same(tag, core.seen.get(count - 1));
    }
    private static void metadata(boolean original) {
        if (Core.class.getGenericInterfaces()[0] != Api.class || Core.class.getTypeParameters().length != 0
                || CORE.getTypeParameters().length != (original ? 2 : 0)
                || ROOT.getTypeParameters().length != 2 || DEFAULT.getTypeParameters().length != 2
                || CORE.getReturnType() != Object[].class || CORE.isBridge() || CORE.isSynthetic())
            throw new AssertionError("raw class scope or method inventory changed");
    }
    public static void main(String[] args) throws Throwable {
        metadata(args[0].equals("original"));
        Object[][] arrays = {null, new Object[0], new Object[]{new Object()}, new String[]{"owned"}, new Integer[]{-1, 0, 1}};
        for (int route = 0; route < 9; route++) {
            for (Object[] array : arrays) {
                Core core = new Core(); int[] tag = {7};
                same(array, call(route, core, array, tag)); check(core, array, tag, 1, 1);
                state(12, tag[0]);
                same(array, call(route, core, array, tag)); check(core, array, tag, 2, 4);
                state(17, tag[0]);
                values++;
            }
            Core core = new Core(); int[] tag = {11}; Object[] input = new Object[]{new Object()};
            RuntimeException failure = new RuntimeException("owned-failure"); core.failure = failure;
            try { call(route, core, input, tag); throw new AssertionError("failure missing"); }
            catch (Throwable error) { same(failure, error); failures++; }
            check(core, input, tag, 1, 1); state(16, tag[0]);
            try { call(route, null, input, tag); throw new AssertionError("null receiver accepted"); }
            catch (NullPointerException error) { failures++; }
            check(core, input, tag, 1, 1); state(16, tag[0]);
            same(null, call(route, new Core(), null, null));
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("raw-interface-and-runtime-body=retained,independent-method-formal-source-erasure=reported");
    }
}
