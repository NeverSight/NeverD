import fixture.*;
import java.lang.reflect.*;

public final class ParameterReturnCovarianceHarness {
    private static int values, identities, states, failures;
    private static final Object MARKER = new Object();
    private interface Call { Object run(Object key, Object value) throws Throwable; }
    private static final class Leaf extends Core {
        public Core put(String key, Object value) throws First, Second {
            calls += 2;
            return super.put(key, value);
        }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original parameter/return covariance identity changed");
        identities++;
    }
    private static Object invoke(Method method, Object receiver, Object key, Object value) throws Throwable {
        try { return method.invoke(receiver, key, value); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Call[] paths(Core receiver) throws Exception {
        Root<String, Object> typedRoot = receiver;
        Root rawRoot = receiver;
        Api<String, Object> typedApi = receiver;
        Api rawApi = receiver;
        Method typed = Core.class.getDeclaredMethod("put", String.class, Object.class);
        Method bridge = Core.class.getDeclaredMethod("put", Object.class, Object.class);
        Method parent = Root.class.getDeclaredMethod("put", Object.class, Object.class);
        Method required = Api.class.getDeclaredMethod("put", Object.class, Object.class);
        return new Call[] {
            (key, value) -> receiver.put((String) key, value),
            (key, value) -> typedRoot.put((String) key, value),
            (key, value) -> rawRoot.put(key, value),
            (key, value) -> typedApi.put((String) key, value),
            (key, value) -> rawApi.put(key, value),
            (key, value) -> invoke(typed, receiver, key, value),
            (key, value) -> invoke(bridge, receiver, key, value),
            (key, value) -> invoke(parent, receiver, key, value),
            (key, value) -> invoke(required, receiver, key, value)
        };
    }
    private static void reset(Core receiver, First first, Second second) {
        receiver.calls = 0; receiver.seenKey = null; receiver.seenValue = null;
        receiver.failure = first; receiver.second = second;
    }
    private static void state(Core receiver, int calls, String key, Object value,
                              First first, Second second, Core result) {
        if (receiver.calls != calls) throw new AssertionError("original parameter/return covariance effects changed");
        same(receiver.seenKey, key); same(receiver.seenValue, value);
        same(receiver.failure, first); same(receiver.second, second); same(receiver.result, result);
        states++;
    }
    public static void main(String[] args) throws Throwable {
        Method typed = Core.class.getDeclaredMethod("put", String.class, Object.class);
        Method bridge = Core.class.getDeclaredMethod("put", Object.class, Object.class);
        Method parent = Root.class.getDeclaredMethod("put", Object.class, Object.class);
        if (Core.class.getSuperclass() != Root.class || Core.class.getTypeParameters().length != 0
                || !Core.class.getGenericSuperclass().getTypeName().equals("fixture.Root<java.lang.String, java.lang.Object>")
                || Root.class.getTypeParameters().length != 2 || Api.class.getTypeParameters().length != 2
                || typed.getReturnType() != Core.class || typed.isBridge() || typed.getTypeParameters().length != 0
                || !bridge.isBridge() || !bridge.isSynthetic() || bridge.getReturnType() != Object.class
                || bridge.getExceptionTypes().length != 0 || typed.getExceptionTypes().length != 2
                || typed.getExceptionTypes()[0] != First.class || typed.getExceptionTypes()[1] != Second.class
                || !parent.getGenericParameterTypes()[0].getTypeName().equals("K")
                || !parent.getGenericParameterTypes()[1].getTypeName().equals("V")
                || !parent.getGenericReturnType().getTypeName().equals("V"))
            throw new AssertionError("original full substitutions, unchecked Exceptions or erased entry changed");
        String key = new String("owned key");
        for (Core receiver : new Core[] {new Core(), new Leaf()}) {
            int delta = receiver instanceof Leaf ? 3 : 1;
            Call[] routes = paths(receiver), absent = paths(null);
            for (int route = 0; route < routes.length; route++) {
                for (String inputKey : new String[] {null, key}) {
                    Object[] inputs = {null, new Object(), new Object[] {MARKER}};
                    for (int mode = 0; mode < inputs.length; mode++) {
                        Object input = inputs[mode];
                        Core result = mode == 0 ? null : mode == 1 ? new Core() : receiver;
                        receiver.result = result; reset(receiver, null, null);
                        same(routes[route].run(inputKey, input), result);
                        state(receiver, delta, inputKey, input, null, null, result);
                        if (input instanceof Object[]) {
                            ((Object[]) input)[0] = receiver;
                            same(((Object[]) receiver.seenValue)[0], receiver);
                        }
                        values++;
                    }
                }
                Core result = receiver.result;
                for (int mode = 0; mode < 5; mode++) {
                    First first = mode == 0 || mode == 3 ? new First() : null;
                    Second second = mode == 1 ? new Second() : null;
                    Object input = new Object[] {MARKER};
                    Object inputKey = mode == 2 || mode == 3 ? new Object() : key;
                    reset(receiver, first, second);
                    try {
                        (mode == 4 ? absent[route] : routes[route]).run(inputKey, input);
                        throw new AssertionError("original parameter/return covariance failure missing");
                    } catch (Throwable error) {
                        Object expected = mode == 0 ? first : mode == 1 ? second : mode == 4
                            ? NullPointerException.class : route == 5 ? IllegalArgumentException.class : ClassCastException.class;
                        same(mode < 2 ? error : error.getClass(), expected); failures++;
                    }
                    state(receiver, mode < 2 ? delta : 0, mode < 2 ? key : null,
                          mode < 2 ? input : null, first, second, result);
                }
            }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("complete-parameter-and-return-substitutions=retained,unchecked-target-order=retained,original-checked-bridge-dispatch=retained");
    }
}
