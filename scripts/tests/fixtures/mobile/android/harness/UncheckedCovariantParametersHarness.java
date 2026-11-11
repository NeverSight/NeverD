import fixture.*;
import java.lang.reflect.*;

public final class UncheckedCovariantParametersHarness {
    private static int values, identities, states, failures;
    private interface Call { Object run(Object marker, String name, Carrier input) throws Throwable; }
    private static final class Leaf extends Core {
        public Core apply(Object marker, String name, Carrier<Number, String> input) throws First, Second {
            calls += 2;
            return super.apply(marker, name, input);
        }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original unchecked covariance identity changed");
        identities++;
    }
    private static Method entry(boolean bridge) throws Exception {
        for (Method method : Core.class.getDeclaredMethods())
            if (method.getName().equals("apply") && method.isBridge() == bridge) return method;
        throw new AssertionError("original typed or erased entry missing");
    }
    private static Object invoke(Method method, Object receiver, Object marker, String name,
                                 Carrier input) throws Throwable {
        try { return method.invoke(receiver, marker, name, input); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Call[] paths(Core receiver) throws Exception {
        Api api = receiver;
        Method typed = entry(false), bridge = entry(true);
        Method required = Api.class.getDeclaredMethod("apply", Object.class, String.class, Carrier.class);
        return new Call[] {
            (marker, name, input) -> receiver.apply(marker, name, input),
            (marker, name, input) -> api.apply(marker, name, input),
            (marker, name, input) -> invoke(typed, receiver, marker, name, input),
            (marker, name, input) -> invoke(bridge, receiver, marker, name, input),
            (marker, name, input) -> invoke(required, receiver, marker, name, input)
        };
    }
    private static void reset(Core receiver, First first, Second second) {
        receiver.calls = 0; receiver.seenMarker = null; receiver.seenName = null;
        receiver.seenInput = null; receiver.seenValue = null; receiver.seenLabel = null;
        receiver.failure = first; receiver.second = second;
    }
    private static void state(Core receiver, int calls, Object marker, String name, Carrier input,
                              Number value, String label, First first, Second second, Core result) {
        if (receiver.calls != calls) throw new AssertionError("original unchecked covariance effects changed");
        same(receiver.seenMarker, marker); same(receiver.seenName, name); same(receiver.seenInput, input);
        same(receiver.seenValue, value); same(receiver.seenLabel, label);
        same(receiver.failure, first); same(receiver.second, second); same(receiver.result, result);
        states++;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        Method typed = entry(false), bridge = entry(true);
        if (Core.class.getTypeParameters().length != 0 || Core.class.getInterfaces()[0] != Api.class
                || typed.getReturnType() != Core.class || typed.isBridge()
                || typed.getTypeParameters().length != 0 || bridge.getReturnType() != Api.class
                || !bridge.isBridge() || !bridge.isSynthetic() || bridge.getExceptionTypes().length != 0
                || typed.getExceptionTypes().length != 2 || typed.getExceptionTypes()[0] != First.class
                || typed.getExceptionTypes()[1] != Second.class
                || !typed.getGenericParameterTypes()[2].getTypeName()
                    .equals("fixture.Carrier<java.lang.Number, java.lang.String>")
                || Api.class.getDeclaredMethod("apply", Object.class, String.class, Carrier.class)
                    .getExceptionTypes().length != 0)
            throw new AssertionError("original unchecked Exceptions, complete parameters or runtime bridge changed");
        Object marker = new Object(); String name = new String("owned name"), label = new String("owned label");
        Number number = new java.util.concurrent.atomic.AtomicInteger(7);
        for (Core receiver : new Core[] {new Core(), new Leaf()}) {
            int delta = receiver instanceof Leaf ? 3 : 1;
            Call[] routes = paths(receiver), absent = paths(null);
            for (int route = 0; route < routes.length; route++) {
                for (Core result : new Core[] {new Core(), null, receiver}) {
                    Carrier<Number, String> input = new Carrier<>(number, label);
                    receiver.result = result; reset(receiver, null, null);
                    same(routes[route].run(marker, name, input), result);
                    state(receiver, delta, marker, name, input, number, label, null, null, result);
                    Number changed = new java.util.concurrent.atomic.AtomicInteger(9);
                    String changedLabel = new String("changed label");
                    input.value = changed; input.label = changedLabel;
                    same(receiver.seenInput.value, changed); same(receiver.seenInput.label, changedLabel);
                    values++;
                }
                Core result = receiver.result;
                for (int mode = 0; mode < 7; mode++) {
                    First first = mode == 0 || mode == 5 ? new First() : null;
                    Second second = mode == 1 ? new Second() : null;
                    Carrier input = mode == 2 ? null : mode == 3 || mode == 5
                        ? new Carrier(new Object(), label) : mode == 4
                        ? new Carrier(number, new Object()) : new Carrier(number, label);
                    reset(receiver, first, second);
                    try {
                        (mode == 6 ? absent[route] : routes[route]).run(marker, name, input);
                        throw new AssertionError("original ordered covariance failure missing");
                    } catch (Throwable error) {
                        Object expected = mode == 0 ? first : mode == 1 ? second
                            : mode == 2 || mode == 6 ? NullPointerException.class : ClassCastException.class;
                        same(mode < 2 ? error : error.getClass(), expected); failures++;
                    }
                    state(receiver, mode == 6 ? 0 : delta, mode == 6 ? null : marker,
                          mode == 6 ? null : name, mode == 6 ? null : input,
                          mode < 2 || mode == 4 ? number : null, mode < 2 ? label : null,
                          first, second, result);
                }
            }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("unchecked-target-order=retained,complete-parameterized-contract=retained,original-bridge-dispatch=retained");
    }
}
