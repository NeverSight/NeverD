import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class ClassFormalImplementationBridgesHarness {
    private static int values, identities, states, failures;
    private static final Method OWN, API;
    private static final MethodHandle HANDLE;
    static {
        try {
            OWN = Core.class.getMethod("contains", Comparable.class);
            API = Api.class.getMethod("contains", Comparable.class);
            HANDLE = MethodHandles.lookup().findVirtual(Core.class, "contains",
                    MethodType.methodType(boolean.class, Comparable.class));
        } catch (Exception error) { throw new AssertionError(error); }
    }
    private static final class Item implements Comparable<Item> {
        final int value; int calls; Item observed; RuntimeException failure;
        Item(int value) { this.value = value; }
        public int compareTo(Item other) {
            calls++; observed = other;
            if (failure != null) throw failure;
            return Integer.compare(value, other.value);
        }
    }
    private static final class Derived extends Core<Item,Item> {
        public Item observe(Item value) { marker[0] += 5; return super.observe(value); }
    }
    private static void same(Object expected, Object actual) {
        if (expected != actual) throw new AssertionError("identity changed"); identities++;
    }
    private static void state(int expected, int actual) {
        if (expected != actual) throw new AssertionError("state changed " + expected + ":" + actual); states++;
    }
    private static void value(boolean expected, boolean actual) {
        if (expected != actual) throw new AssertionError("boolean changed"); values++;
    }
    private static void check(Core<?,?> receiver, int[] marker, int calls, int step) {
        state(calls, receiver.calls); state(7 + calls * step, marker[0]);
        same(marker, receiver.marker); same(marker, receiver.observed);
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static boolean call(int route, Core receiver, Comparable argument) throws Throwable {
        try {
            switch (route) {
                case 0: return receiver.contains(argument);
                case 1: return ((Api) receiver).contains(argument);
                case 2: return ((Api<Item>) receiver).contains((Item) argument);
                case 3: return (Boolean) OWN.invoke(receiver, argument);
                case 4: return (Boolean) API.invoke(receiver, argument);
                case 5: return (boolean) HANDLE.invokeExact(receiver, argument);
                case 6: return Caller.call(receiver, argument);
                case 7: return Dispatch.contains(receiver, argument);
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void metadata(boolean original) throws Exception {
        if (OWN.isBridge() != original || Core.class.getMethod("isEmpty").isBridge() != original || OWN.isSynthetic()
                || OWN.getTypeParameters().length != 0 || Core.class.getTypeParameters().length != 2
                || !OWN.getGenericParameterTypes()[0].equals(Core.class.getTypeParameters()[0])
                || !Core.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Api<T>"))
            throw new AssertionError("original class-owned method scope changed");
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static void run(int route, boolean derived) throws Throwable {
        Core<Item,Item> receiver = derived ? new Derived() : new Core<>();
        int[] marker = {7}; receiver.marker = marker;
        int step = derived ? 8 : 3;
        Item low = new Item(5), below = new Item(3), equal = new Item(5), above = new Item(7);
        receiver.bound = low;
        value(false, receiver.isEmpty());
        int calls = 0;
        for (Item input : new Item[] {below, equal, above}) {
            value(input.value >= low.value, call(route, receiver, input));
            check(receiver, marker, ++calls, step); same(input, receiver.seen);
            state(1, input.calls); same(low, input.observed);
        }
        value(true, call(route, receiver, low));
        check(receiver, marker, ++calls, step); same(low, receiver.seen); same(low, low.observed);
        try { call(route, receiver, null); throw new AssertionError("null value accepted"); }
        catch (NullPointerException expected) { failures++; }
        check(receiver, marker, ++calls, step); same(null, receiver.seen);
        RuntimeException compare = new RuntimeException("compare"); above.failure = compare;
        try { call(route, receiver, above); throw new AssertionError("comparison failure missing"); }
        catch (RuntimeException error) { same(compare, error); failures++; }
        check(receiver, marker, ++calls, step); state(2, above.calls); above.failure = null;
        // A typed immutable caller and a specialized callback may check earlier.
        int beforeWrong = calls;
        try { call(route, receiver, "wrong"); throw new AssertionError("wrong class accepted"); }
        catch (ClassCastException expected) { failures++; }
        if (derived || route == 2) {
            check(receiver, marker, beforeWrong, step);
        } else {
            check(receiver, marker, ++calls, step); same("wrong", ((Core) receiver).seen);
        }
        RuntimeException callback = new RuntimeException("callback"); receiver.failure = callback;
        try { call(route, receiver, below); throw new AssertionError("callback failure missing"); }
        catch (RuntimeException error) { same(callback, error); failures++; }
        check(receiver, marker, ++calls, step); state(1, below.calls); receiver.failure = null;
        value(false, call(route, receiver, below));
        check(receiver, marker, ++calls, step); state(2, below.calls);
        same(low, receiver.lower()); same(low, ((Api) receiver).lower());
        try { call(route, null, below); throw new AssertionError("null receiver accepted"); }
        catch (NullPointerException expected) { failures++; }
        check(receiver, marker, calls, step);
        receiver.bound = null; value(true, receiver.isEmpty());
    }
    public static void main(String[] args) throws Throwable {
        metadata(args[0].equals("original"));
        for (int route = 0; route < 8; route++) {
            run(route, false); run(route, true);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("complete-class-and-method-scopes=retained,original-helper-and-virtual-callback-order=retained,bridge-flag-difference=reported");
    }
}
