import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class ComparableScalarPredicatesHarness {
    private static int values, identities, states, failures;
    private static final Method OWN, API, OTHER;
    private static final MethodHandle HANDLE;
    static {
        try {
            OWN = Core.class.getMethod("contains", Comparable.class);
            API = Api.class.getMethod("contains", Comparable.class);
            OTHER = OtherApi.class.getMethod("contains", Comparable.class);
            HANDLE = MethodHandles.lookup().findVirtual(Core.class, "contains",
                    MethodType.methodType(boolean.class, Comparable.class));
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
    @SuppressWarnings({"unchecked", "rawtypes"})
    private static boolean call(int route, Core receiver, Comparable value) throws Throwable {
        try {
            switch (route) {
                case 0: return receiver.contains(value);
                case 1: return ((Api) receiver).contains(value);
                case 2: return ((OtherApi) receiver).contains(value);
                case 3: return (Boolean) OWN.invoke(receiver, value);
                case 4: return (Boolean) API.invoke(receiver, value);
                case 5: return (Boolean) OTHER.invoke(receiver, value);
                case 6: return (boolean) HANDLE.invokeExact(receiver, value);
                case 7: return Caller.call(receiver, value);
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void metadata(boolean original) throws Exception {
        if (OWN.isBridge() != original || OWN.isSynthetic() != original
                || OWN.getGenericParameterTypes()[0] != Comparable.class
                || java.util.Arrays.stream(Core.class.getDeclaredMethods()).filter(method -> Modifier.isPublic(method.getModifiers())).count() != 5
                || Core.class.getTypeParameters().length != 0)
            throw new AssertionError("bridge projection or runtime inventory changed");
        for (Type edge : Core.class.getGenericInterfaces()) {
            if (original) {
                if (!(edge instanceof ParameterizedType)
                        || ((ParameterizedType) edge).getActualTypeArguments()[0] != Character.class)
                    throw new AssertionError("original interface argument changed");
            } else if (!(edge instanceof Class<?>))
                throw new AssertionError("raw source interface not disclosed");
        }
    }
    @SuppressWarnings({"unchecked", "rawtypes", "removal"})
    private static void run(int route) throws Throwable {
        Core receiver = new Core(); int[] marker = {7}; receiver.marker = marker;
        Character value = new Character('m'); receiver.value = value;
        for (char input : new char[] {0, 'a', 'm', 'z', 127, 65535}) {
            boolean actual = call(route, receiver, Character.valueOf(input));
            if (actual != (input >= 'a' && input <= 'z')) throw new AssertionError("value changed");
            values++;
        }
        state(6, receiver.calls); state(25, marker[0]); same(marker, receiver.observed);
        same(value, receiver.get()); same(value, ((Api) receiver).get()); same(value, ((OtherApi) receiver).get());
        if (receiver.isEmpty()) throw new AssertionError("ordinary method changed"); values++;
        for (Comparable wrong : new Comparable[] {null, "wrong", Integer.valueOf(4)}) {
            try { call(route, receiver, wrong); throw new AssertionError("bad input accepted"); }
            catch (NullPointerException error) { if (wrong != null) throw error; failures++; }
            catch (ClassCastException error) { if (wrong == null) throw error; failures++; }
            state(6, receiver.calls); state(25, marker[0]);
        }
        RuntimeException failure = new RuntimeException("owned"); receiver.failure = failure;
        try { call(route, receiver, value); throw new AssertionError("original failure missing"); }
        catch (RuntimeException error) { same(failure, error); failures++; }
        state(7, receiver.calls); state(28, marker[0]); same(marker, receiver.observed);
        receiver.failure = null;
        if (!Caller.typed((Api<Character>) (Api) receiver, value)) throw new AssertionError("typed caller changed"); values++;
        if (!((OtherApi<Character>) (OtherApi) receiver).contains(value)) throw new AssertionError("typed interface changed"); values++;
        state(9, receiver.calls); state(34, marker[0]);
        receiver.value = null; same(null, ((Api) receiver).get());
        if (!receiver.isEmpty()) throw new AssertionError("null getter changed"); values++;
        try { call(route, null, value); throw new AssertionError("null receiver accepted"); }
        catch (NullPointerException expected) { failures++; }
        state(9, receiver.calls); state(34, marker[0]); same(marker, receiver.marker);
    }
    public static void main(String[] args) throws Throwable {
        metadata(args[0].equals("original"));
        for (int route = 0; route < 8; route++) run(route);
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-casts-unboxing-parent-dispatch-and-runtime-slots=retained,raw-interface-and-bridge-metadata=reported");
    }
}
