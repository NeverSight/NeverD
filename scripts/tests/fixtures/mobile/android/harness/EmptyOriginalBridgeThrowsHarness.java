import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.Arrays;

public final class EmptyOriginalBridgeThrowsHarness {
    private static int values, identities, states, failures;
    private static final Method TYPED, BRIDGE, PARENT, API;
    private static final MethodHandle TYPED_HANDLE, BRIDGE_HANDLE;
    static {
        try {
            TYPED = Core.class.getMethod("visit", Box.class, int[].class);
            BRIDGE = Core.class.getMethod("visit", Object.class, int[].class);
            PARENT = Parent.class.getMethod("visit", Object.class, int[].class);
            API = Api.class.getMethod("visit", Object.class, int[].class);
            TYPED_HANDLE = MethodHandles.lookup().findVirtual(Core.class, "visit", MethodType.methodType(Box.class, Box.class, int[].class));
            BRIDGE_HANDLE = MethodHandles.lookup().findVirtual(Core.class, "visit", MethodType.methodType(Box.class, Object.class, int[].class));
        } catch (Exception error) { throw new AssertionError(error); }
    }
    private static void same(Object expected, Object actual) {
        if (expected != actual) throw new AssertionError("identity changed");
        identities++;
    }
    private static void state(int expected, int actual) {
        if (expected != actual) throw new AssertionError("state changed " + expected + ":" + actual);
        states++;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Object call(int route, Core receiver, Object value, int[] marker) throws Throwable {
        try {
            switch (route) {
                case 0: return receiver.visit((Box) value, marker);
                case 1: return ((Parent) receiver).visit(value, marker);
                case 2: return ((Api) receiver).visit(value, marker);
                case 3: return TYPED.invoke(receiver, (Box) value, marker);
                case 4: return BRIDGE.invoke(receiver, value, marker);
                case 5: return PARENT.invoke(receiver, value, marker);
                case 6: return API.invoke(receiver, value, marker);
                case 7: return (Box) TYPED_HANDLE.invokeExact(receiver, (Box) value, marker);
                case 8: return (Box) BRIDGE_HANDLE.invokeExact(receiver, value, marker);
                case 9: return Caller.typed(receiver, (Box) value, marker);
                case 10: return Caller.parent(receiver, (Box) value, marker);
                case 11: return Caller.api(receiver, (Box) value, marker);
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void metadata(boolean original) {
        Class<?>[] expected = {First.class, Second.class};
        if (!BRIDGE.isBridge() || !BRIDGE.isSynthetic() || TYPED.isBridge() || TYPED.isSynthetic()
                || TYPED.getExceptionTypes().length != 0
                || !Arrays.equals(BRIDGE.getExceptionTypes(), original ? new Class<?>[0] : expected)
                || !Arrays.equals(PARENT.getExceptionTypes(), expected) || !Arrays.equals(API.getExceptionTypes(), expected)
                || !Core.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<fixture.Box>")
                || PARENT.getGenericReturnType() != Box.class
                || !PARENT.getGenericParameterTypes()[0].getTypeName().equals("T"))
            throw new AssertionError("original contracts or reported bridge exception metadata changed");
    }
    private static void reset(Core core) {
        core.calls = core.visits = 0; core.seen = null; core.seenParent = null; core.seenMarker = null;
        core.first = null; core.second = null;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static void run(int route) throws Throwable {
        Core core = new Core(); Box input = new Box(), result = new Box(); int[] marker = {7};
        input.payload = marker; result.payload = input; core.result = result;
        same(result, call(route, core, input, marker)); same(result, call(route, core, input, marker));
        state(2, core.calls); state(2, core.visits); state(35, marker[0]);
        same(input, core.seen); same(input, core.seenParent); same(marker, core.seenMarker);
        same(marker, core.seen.payload); same(input, result.payload);
        result.payload = core; same(core, ((Box) core.result).payload);
        core.result = null; same(null, call(route, core, null, marker));
        state(3, core.calls); state(3, core.visits); state(49, marker[0]);
        same(null, core.seen); same(null, core.seenParent); values += 3;
        for (int mode = 0; mode < 6; mode++) {
            reset(core); marker[0] = 7; core.result = result;
            First first = new First(); Second second = new Second();
            if (mode == 0) core.first = first;
            if (mode == 1) core.second = second;
            if (mode == 3) ((Parent) core).result = new Object();
            Object argument = mode == 4 ? new Object() : input;
            Core receiver = mode == 5 ? null : core;
            int[] mark = mode == 2 ? null : marker;
            try { call(route, receiver, argument, mark); throw new AssertionError("missing original failure"); }
            catch (Throwable error) {
                Object wanted = mode == 0 ? first : mode == 1 ? second
                    : mode == 2 || mode == 5 ? NullPointerException.class : ClassCastException.class;
                same(wanted, mode < 2 ? error : error.getClass()); failures++;
            }
            int entered = mode < 4 ? 1 : 0;
            state(entered, core.calls); state(entered, core.visits);
            state(mode < 4 && mode != 2 ? 10 : 7, marker[0]);
            same(entered == 1 ? input : null, core.seen);
            same(entered == 1 ? input : null, core.seenParent);
            same(entered == 1 ? mark : null, core.seenMarker);
            same(mode == 0 ? first : null, core.first); same(mode == 1 ? second : null, core.second);
        }
        reset(core); core.result = result; marker[0] = 5;
        same(result, call(route, core, input, marker));
        state(1, core.calls); state(1, core.visits); state(19, marker[0]); values++;
    }
    public static void main(String[] args) throws Throwable {
        metadata(args[0].equals("original"));
        for (int route = 0; route < 12; route++) run(route);
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-empty-target-throws=retained,bridge-exception-difference=reported,super-and-erased-dispatch=retained");
    }
}
