import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class ComparableCarrierPredicatesHarness {
    private static int values, identities, states, failures;
    private static final Method[] OWN = new Method[3], API = new Method[3];
    private static final MethodHandle[] HANDLES = new MethodHandle[3];
    static {
        try {
            Class<?>[][] parameters = {{Comparable.class}, {Comparable.class, Comparable.class}, {}};
            String[] names = {"contains", "before", "get"};
            for (int i = 0; i < 3; i++) {
                OWN[i] = Core.class.getMethod(names[i], parameters[i]);
                API[i] = Api.class.getMethod(names[i], parameters[i]);
                HANDLES[i] = MethodHandles.lookup().findVirtual(Core.class, names[i],
                        MethodType.methodType(i == 2 ? Comparable.class : boolean.class, parameters[i]));
            }
        } catch (Exception error) { throw new AssertionError(error); }
    }
    private static void same(Object expected, Object actual) {
        if (expected != actual) throw new AssertionError("identity changed"); identities++;
    }
    private static void state(long expected, long actual) {
        if (expected != actual) throw new AssertionError("state changed " + expected + ":" + actual); states++;
    }
    private static void value(boolean expected, boolean actual) {
        if (expected != actual) throw new AssertionError("value changed"); values++;
    }
    private static Object call(int route, int operation, Core owner, Comparable left, Comparable right) throws Throwable {
        try {
            switch (route) {
                case 0: return operation == 0 ? owner.contains(left) : operation == 1 ? owner.before(left, right) : owner.get();
                case 1: return operation == 0 ? ((Api) owner).contains(left) : operation == 1 ? ((Api) owner).before(left, right) : ((Api) owner).get();
                case 2: return operation == 0 ? ((OtherApi) owner).contains(left) : operation == 1 ? ((OtherApi) owner).before(left, right) : ((OtherApi) owner).get();
                case 3: return operation == 0 ? ((RootApi) owner).contains(left) : operation == 1 ? ((Api) owner).before(left, right) : ((RootApi) owner).get();
                case 4: return OWN[operation].invoke(owner, operation == 0 ? new Object[]{left} : operation == 1 ? new Object[]{left, right} : new Object[]{});
                case 5: return API[operation].invoke(owner, operation == 0 ? new Object[]{left} : operation == 1 ? new Object[]{left, right} : new Object[]{});
                case 6:
                    if (operation == 0) return (boolean) HANDLES[0].invokeExact(owner, left);
                    if (operation == 1) return (boolean) HANDLES[1].invokeExact(owner, left, right);
                    return (Comparable) HANDLES[2].invokeExact(owner);
                case 7: return operation == 0 ? Caller.contains(owner, left) : operation == 1 ? Caller.before(owner, left, right) : Caller.get(owner);
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void check(Core owner, int[] marker, int calls, int delta) {
        state(calls, owner.calls); state(7 + delta, marker[0]);
        same(marker, owner.marker); same(calls == 0 ? null : marker, owner.observed);
    }
    private static void failure(int route, int operation, Core owner, Comparable left, Comparable right,
                                Class<? extends Throwable> kind, Throwable exact) throws Throwable {
        try { call(route, operation, owner, left, right); throw new AssertionError("missing failure"); }
        catch (Throwable error) {
            if (!kind.isInstance(error)) throw error;
            if (exact != null) same(exact, error);
            failures++;
        }
    }
    private static void run(int route) throws Throwable {
        Core owner = new Core(); int[] marker = {7}; owner.marker = marker;
        int calls = 0, delta = 0;
        Carrier.reads = 0; Carrier.factories = 0; Carrier.factoryFailure = null; Carrier.last = null;
        for (long number : new long[]{Long.MIN_VALUE, -1, 0, 1, Long.MAX_VALUE}) {
            Carrier input = new Carrier(number);
            value(number >= 0, (Boolean) call(route, 0, owner, input, null));
            check(owner, marker, ++calls, delta += 3);
            state(1, input.calls); state(number, owner.seenLeft);
        }
        Carrier left = new Carrier(Long.MIN_VALUE), right = new Carrier(Long.MAX_VALUE);
        value(true, (Boolean) call(route, 1, owner, left, right));
        check(owner, marker, ++calls, delta += 5);
        state(6, left.order); state(7, right.order); state(left.value, owner.seenLeft); state(right.value, owner.seenRight);
        value(false, (Boolean) call(route, 1, owner, right, left));
        check(owner, marker, ++calls, delta += 5);
        state(8, right.order); state(9, left.order);
        value(false, (Boolean) call(route, 1, owner, left, left));
        check(owner, marker, ++calls, delta += 5); state(11, left.order); state(4, left.calls);
        failure(route, 0, owner, "wrong", null, ClassCastException.class, null);
        check(owner, marker, calls, delta); state(11, Carrier.reads);
        failure(route, 0, owner, null, null, NullPointerException.class, null);
        check(owner, marker, calls, delta); state(11, Carrier.reads);
        failure(route, 1, owner, left, "wrong", ClassCastException.class, null);
        check(owner, marker, calls, delta); state(12, Carrier.reads); state(12, left.order);
        failure(route, 1, owner, left, null, NullPointerException.class, null);
        check(owner, marker, calls, delta); state(13, Carrier.reads);
        RuntimeException conversion = new RuntimeException("conversion"); left.failure = conversion;
        failure(route, 1, owner, left, right, RuntimeException.class, conversion);
        check(owner, marker, calls, delta); state(14, Carrier.reads); state(8, right.order); left.failure = null;
        right.failure = conversion;
        failure(route, 1, owner, left, right, RuntimeException.class, conversion);
        check(owner, marker, calls, delta); state(15, left.order); state(16, right.order); right.failure = null;
        RuntimeException target = new RuntimeException("target"); owner.failure = target;
        failure(route, 1, owner, left, right, RuntimeException.class, target);
        check(owner, marker, ++calls, delta += 5); state(17, left.order); state(18, right.order); owner.failure = null;
        value(true, (Boolean) call(route, 1, owner, left, right));
        check(owner, marker, ++calls, delta += 5); state(19, left.order); state(20, right.order);
        failure(route, 0, null, left, null, NullPointerException.class, null);
        check(owner, marker, calls, delta); state(20, Carrier.reads);
        owner.value = Long.MIN_VALUE;
        Carrier first = (Carrier) call(route, 2, owner, null, null);
        state(owner.value, first.value); state(1, owner.reads); state(1, Carrier.factories);
        same(first, Carrier.last); state(owner.value, Carrier.factoryInput);
        Carrier second = (Carrier) call(route, 2, owner, null, null);
        value(true, first != second); same(second, Carrier.last); state(2, owner.reads); state(2, Carrier.factories);
        RuntimeException factory = new RuntimeException("factory"); Carrier.factoryFailure = factory;
        failure(route, 2, owner, null, null, RuntimeException.class, factory);
        state(3, owner.reads); state(3, Carrier.factories); same(second, Carrier.last); Carrier.factoryFailure = null;
        owner.readFailure = target;
        failure(route, 2, owner, null, null, RuntimeException.class, target);
        state(4, owner.reads); state(3, Carrier.factories); same(second, Carrier.last); owner.readFailure = null;
        failure(route, 2, null, null, null, NullPointerException.class, null);
        state(4, owner.reads); state(3, Carrier.factories);
        value(false, owner.isEmpty()); owner.value = 0; value(true, ((Api) owner).isEmpty());
    }
    private static void metadata(boolean original) {
        for (Method method : OWN)
            if (method.isBridge() != original || method.isSynthetic() != original)
                throw new AssertionError("bridge flag difference not reported");
        Type[] parents = Core.class.getGenericInterfaces();
        if (parents.length != 2 || !(Core.class.getGenericSuperclass() instanceof ParameterizedType))
            throw new AssertionError("unrelated generic metadata changed");
        for (Type parent : parents) {
            if (original) {
                if (!(parent instanceof ParameterizedType)) throw new AssertionError("original carrier binding missing");
                same(Carrier.class, ((ParameterizedType) parent).getActualTypeArguments()[0]);
            } else {
                if (!(parent instanceof Class)) throw new AssertionError("raw source edge missing");
                identities++;
            }
        }
    }
    private static void inlinedCarrier(boolean original) throws Throwable {
        Method compare = Carrier.class.getMethod("compareTo", Object.class);
        if (compare.isBridge() != original || compare.isSynthetic() != original)
            throw new AssertionError("original comparison flag difference missing");
        Type edge = Carrier.class.getGenericInterfaces()[0];
        same(original ? Carrier.class : Object.class, ((ParameterizedType) edge).getActualTypeArguments()[0]);
        MethodHandle handle = MethodHandles.lookup().findVirtual(Carrier.class, "compareTo", MethodType.methodType(int.class, Object.class));
        Carrier low = new Carrier(Long.MIN_VALUE), high = new Carrier(Long.MAX_VALUE);
        for (int route = 0; route < 3; route++) {
            for (Object other : new Object[]{low, high, "wrong", null}) {
                try {
                    int result = route == 0 ? ((Comparable) low).compareTo(other)
                            : route == 1 ? (Integer) compare.invoke(low, other) : (int) handle.invokeExact(low, other);
                    if (!(other instanceof Carrier)) throw new AssertionError("comparison accepted invalid carrier");
                    value(other == low ? result == 0 : result < 0, true);
                } catch (Throwable error) {
                    if (error instanceof InvocationTargetException) error = error.getCause();
                    if (other == null && error instanceof NullPointerException
                            || other instanceof String && error instanceof ClassCastException) failures++;
                    else throw error;
                }
            }
            Object other = low;
            int result = route == 0 ? ((Comparable) high).compareTo(other)
                    : route == 1 ? (Integer) compare.invoke(high, other) : (int) handle.invokeExact(high, other);
            value(true, result > 0); state(0, low.calls); state(0, high.calls);
        }
    }
    public static void main(String[] args) throws Throwable {
        metadata(args[0].equals("original"));
        if (args[1].equals("inlined")) inlinedCarrier(args[0].equals("original"));
        for (int route = 0; route < 8; route++) run(route);
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-conversions-factories-effects-and-runtime-slots=retained,raw-interface-and-bridge-metadata=reported");
    }
}
