import fixture.*;
import java.lang.reflect.*;

public final class SpecializedInterfaceCovarianceHarness {
    private static int values, identities, states, failures;
    private static final Number SENTINEL = Integer.valueOf(173);
    private static final String BEFORE = new String("before");
    private static final String VISITED = "visited";
    private interface Call { Object run(Carrier<Number, String> carrier) throws Throwable; }
    private static final class Leaf extends Core {
        Leaf() { super(null); }
        public Byte get(Carrier<Number, String> carrier) {
            calls += 2;
            return super.get(carrier);
        }
    }
    private static Object invoke(Method method, Object receiver, Object arg) throws Throwable {
        try { return method.invoke(receiver, arg); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original covariant identity changed");
        identities++;
    }
    private static void reset(Core receiver, Byte value, RuntimeException failure) {
        receiver.value = value; receiver.calls = 0; receiver.seen = null;
        receiver.observed = SENTINEL; receiver.failure = failure;
    }
    private static void state(Core receiver, int calls, Object seen, Number observed,
                              Byte value, RuntimeException failure) {
        if (receiver.calls != calls) throw new AssertionError("original virtual dispatch changed");
        same(receiver.seen, seen); same(receiver.observed, observed);
        same(receiver.value, value); same(receiver.failure, failure);
        states++;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        Method typed = null, bridge = null;
        for (Method method : Core.class.getDeclaredMethods())
            if (method.getName().equals("get")) {
                if (method.isBridge()) bridge = method; else typed = method;
            }
        if (typed == null || bridge == null || !bridge.isSynthetic()
                || typed.getReturnType() != Byte.class || bridge.getReturnType() != Object.class
                || typed.getTypeParameters().length != 0 || bridge.getTypeParameters().length != 0
                || !typed.getGenericParameterTypes()[0].getTypeName()
                    .equals("fixture.Carrier<java.lang.Number, java.lang.String>")
                || bridge.getGenericParameterTypes()[0] != Carrier.class
                || typed.getExceptionTypes().length != 0 || bridge.getExceptionTypes().length != 0
                || !Core.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Contract<java.lang.Number>"))
            throw new AssertionError("original full interface, parameter or bridge metadata changed");
        final Method exact = typed, erased = bridge;
        for (Core receiver : new Core[] {new Core(null), new Leaf()}) {
            int delta = receiver instanceof Leaf ? 3 : 1;
            Contract<Number> view = receiver;
            Contract raw = receiver;
            Call[] paths = {receiver::get, view::get, carrier -> raw.get(carrier),
                            carrier -> invoke(exact, receiver, carrier),
                            carrier -> invoke(erased, receiver, carrier)};
            for (Call path : paths) {
                for (Byte value : new Byte[] {Byte.valueOf((byte) 42), Byte.valueOf((byte) -17), null})
                    for (boolean present : new boolean[] {true, false}) {
                        Carrier<Number, String> carrier = present ? new Carrier<Number, String>(SENTINEL, BEFORE) : null;
                        reset(receiver, value, null);
                        Object result = path.run(carrier);
                        same(result, value);
                        if (result != null && ((Number) result).intValue() != value.intValue())
                            throw new AssertionError("SDK Number result dispatch changed");
                        state(receiver, delta, carrier, SENTINEL, value, null);
                        if (present) { same(carrier.first, SENTINEL); same(carrier.second, VISITED); }
                        values++;
                    }
                Byte value = Byte.valueOf((byte) 63);
                Carrier<Number, String> carrier = new Carrier<Number, String>(SENTINEL, BEFORE);
                RuntimeException problem = new IllegalStateException("owned failure");
                reset(receiver, value, problem);
                try { path.run(carrier); throw new AssertionError("original failure missing"); }
                catch (Throwable error) { same(error, problem); failures++; }
                state(receiver, delta, carrier, SENTINEL, value, problem);
                same(carrier.second, BEFORE);

                Object wrong = new Object();
                Carrier bad = new Carrier(wrong, BEFORE);
                reset(receiver, value, null);
                try { path.run((Carrier<Number, String>) bad); throw new AssertionError("late field cast missing"); }
                catch (Throwable error) { same(error.getClass(), ClassCastException.class); failures++; }
                state(receiver, delta, bad, SENTINEL, value, null);
                same(bad.first, wrong); same(bad.second, BEFORE);
            }
            Call[] absent = {carrier -> ((Core) null).get(carrier),
                             carrier -> ((Contract<Number>) null).get(carrier),
                             carrier -> ((Contract) null).get(carrier),
                             carrier -> invoke(exact, null, carrier),
                             carrier -> invoke(erased, null, carrier)};
            for (Call path : absent) {
                Byte value = Byte.valueOf((byte) 71);
                reset(receiver, value, null);
                try { path.run(null); throw new AssertionError("null receiver failure missing"); }
                catch (Throwable error) { same(error.getClass(), NullPointerException.class); failures++; }
                state(receiver, 0, null, SENTINEL, value, null);
            }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("specialized-result=Number,original-result=Byte,erased-bridge=Object,complete-parameters-and-late-cast-order=retained");
    }
}
