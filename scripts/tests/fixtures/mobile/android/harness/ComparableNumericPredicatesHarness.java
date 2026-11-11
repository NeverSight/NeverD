import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class ComparableNumericPredicatesHarness {
    private static int values, identities, states, failures;
    private static final Method[] OWN = new Method[2], API = new Method[2], OTHER = new Method[2];
    private static final MethodHandle[] HANDLE = new MethodHandle[2];
    static {
        try {
            for (int pair = 0; pair < 2; pair++) {
                String name = pair == 0 ? "contains" : "before";
                Class<?>[] inputs = pair == 0 ? new Class<?>[] {Comparable.class} : new Class<?>[] {Comparable.class, Comparable.class};
                OWN[pair] = Core.class.getMethod(name, inputs);
                API[pair] = Api.class.getMethod(name, inputs);
                OTHER[pair] = OtherApi.class.getMethod(name, inputs);
                HANDLE[pair] = MethodHandles.lookup().findVirtual(Core.class, name, MethodType.methodType(boolean.class, inputs));
            }
        } catch (Exception error) { throw new AssertionError(error); }
    }
    private static void same(Object expected, Object actual) {
        if (expected != actual) throw new AssertionError("identity changed"); identities++;
    }
    private static void state(int expected, int actual) {
        if (expected != actual) throw new AssertionError("state changed " + expected + ":" + actual); states++;
    }
    private static void value(boolean expected, boolean actual) {
        if (expected != actual) throw new AssertionError("numeric result changed"); values++;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static boolean call(int route, Core receiver, Comparable left, Comparable right, boolean pair) throws Throwable {
        int at = pair ? 1 : 0;
        try {
            switch (route) {
                case 0: return pair ? receiver.before(left, right) : receiver.contains(left);
                case 1: return pair ? ((Api) receiver).before(left, right) : ((Api) receiver).contains(left);
                case 2: return pair ? ((OtherApi) receiver).before(left, right) : ((OtherApi) receiver).contains(left);
                case 3: return (Boolean) (pair ? OWN[at].invoke(receiver, left, right) : OWN[at].invoke(receiver, left));
                case 4: return (Boolean) (pair ? API[at].invoke(receiver, left, right) : API[at].invoke(receiver, left));
                case 5: return (Boolean) (pair ? OTHER[at].invoke(receiver, left, right) : OTHER[at].invoke(receiver, left));
                case 6: return pair ? (boolean) HANDLE[at].invokeExact(receiver, left, right) : (boolean) HANDLE[at].invokeExact(receiver, left);
                case 7: return pair ? Caller.pair(receiver, left, right) : Caller.call(receiver, left);
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static final class TraceNumber extends Number implements Comparable<TraceNumber> {
        final int[] trace; final int digit; final double value; RuntimeException failure;
        TraceNumber(int[] trace, int digit, double value) { this.trace = trace; this.digit = digit; this.value = value; }
        public double doubleValue() { trace[0] = trace[0] * 10 + digit; if (failure != null) throw failure; return value; }
        public float floatValue() { throw new AssertionError("wrong conversion"); }
        public long longValue() { throw new AssertionError("wrong conversion"); }
        public int intValue() { throw new AssertionError("wrong conversion"); }
        public int compareTo(TraceNumber other) { throw new AssertionError("invented comparison"); }
    }
    private static void metadata(boolean original) throws Exception {
        for (Method method : OWN)
            if (method.isBridge() != original || method.isSynthetic() != original)
                throw new AssertionError("bridge flag difference changed");
        if (java.util.Arrays.stream(Core.class.getDeclaredMethods()).filter(method -> Modifier.isPublic(method.getModifiers())).count() != 7)
            throw new AssertionError("runtime inventory changed");
        for (Type edge : Core.class.getGenericInterfaces()) {
            if (original) {
                if (!(edge instanceof ParameterizedType) || ((ParameterizedType) edge).getActualTypeArguments()[0] != Double.class)
                    throw new AssertionError("original Double argument changed");
            } else if (!(edge instanceof Class<?>)) throw new AssertionError("source edge not raw");
        }
    }
    @SuppressWarnings({"rawtypes", "unchecked", "removal"})
    private static void run(int route) throws Throwable {
        Core receiver = new Core(); int[] marker = {7}; receiver.marker = marker;
        Double boxed = new Double(109); receiver.value = boxed;
        for (double input : new double[] {Double.NEGATIVE_INFINITY, -0.0, 0.0, 97, 109, 122, 123, Double.POSITIVE_INFINITY, Double.NaN})
            value(input >= 97 && input <= 122, call(route, receiver, Double.valueOf(input), null, false));
        state(9, receiver.calls); state(34, marker[0]);
        value(true, call(route, receiver, Float.valueOf(107.5f), null, false));
        state(10, receiver.calls); state(37, marker[0]);
        same(boxed, receiver.get()); same(boxed, ((Api) receiver).get()); same(boxed, ((OtherApi) receiver).get());
        value(false, receiver.isEmpty());
        for (Comparable wrong : new Comparable[] {null, "wrong", Character.valueOf('a')}) {
            try { call(route, receiver, wrong, null, false); throw new AssertionError("bad input accepted"); }
            catch (NullPointerException error) { if (wrong != null) throw error; failures++; }
            catch (ClassCastException error) { if (wrong == null) throw error; failures++; }
            state(10, receiver.calls); state(37, marker[0]);
        }
        for (double[] input : new double[][] {{Double.NaN,5}, {5,Double.NaN}, {-0.0,0.0}, {Double.NEGATIVE_INFINITY,Double.POSITIVE_INFINITY}, {1,2}, {2,1}})
            value(input[0] <= input[1], call(route, receiver, Double.valueOf(input[0]), Double.valueOf(input[1]), true));
        state(16, receiver.calls); state(55, marker[0]);
        int[] trace = {0}; TraceNumber left = new TraceNumber(trace, 1, 99), right = new TraceNumber(trace, 2, 100);
        value(true, call(route, receiver, left, right, true));
        state(12, trace[0]); state(17, receiver.calls); state(58, marker[0]);
        RuntimeException first = new RuntimeException("first"), second = new RuntimeException("second");
        for (int bad = 0; bad < 6; bad++) {
            trace[0] = 0; left.failure = bad == 0 ? first : null; right.failure = bad == 1 ? second : null;
            Comparable a = bad == 3 ? "wrong" : bad == 4 ? null : left;
            Comparable b = bad == 2 ? "wrong" : bad == 5 ? null : right;
            try { call(route, receiver, a, b, true); throw new AssertionError("ordered failure missing"); }
            catch (RuntimeException error) {
                if (bad == 0) same(first, error);
                else if (bad == 1) same(second, error);
                else if (bad < 4 && !(error instanceof ClassCastException)) throw error;
                else if (bad >= 4 && !(error instanceof NullPointerException)) throw error;
                failures++;
            }
            state(bad == 0 || bad == 2 || bad == 5 ? 1 : bad == 1 ? 12 : 0, trace[0]);
            state(17, receiver.calls); state(58, marker[0]);
        }
        left.failure = null; right.failure = null; trace[0] = 0;
        value(true, call(route, receiver, left, left, true));
        state(11, trace[0]); state(18, receiver.calls); state(61, marker[0]);
        RuntimeException target = new RuntimeException("target"); receiver.failure = target; trace[0] = 0;
        try { call(route, receiver, left, right, true); throw new AssertionError("target failure missing"); }
        catch (RuntimeException error) { same(target, error); failures++; }
        state(12, trace[0]); state(19, receiver.calls); state(64, marker[0]); receiver.failure = null;
        for (boolean pair : new boolean[] {false, true}) {
            trace[0] = 0;
            try { call(route, null, left, right, pair); throw new AssertionError("null receiver accepted"); }
            catch (NullPointerException expected) { failures++; }
            state(0, trace[0]); state(19, receiver.calls); state(64, marker[0]);
        }
        value(true, Caller.typed((Api<Double>) (Api) receiver, boxed));
        value(true, Caller.typedPair((Api<Double>) (Api) receiver, 1.0, 2.0));
        state(21, receiver.calls); state(70, marker[0]); same(marker, receiver.marker); same(marker, receiver.observed);
    }
    public static void main(String[] args) throws Throwable {
        metadata(args[0].equals("original"));
        for (int route = 0; route < 8; route++) run(route);
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-casts-unboxing-parent-dispatch-and-runtime-slots=retained,raw-interface-and-bridge-metadata=reported");
    }
}
