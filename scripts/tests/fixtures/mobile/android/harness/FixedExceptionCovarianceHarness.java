import fixture.*;
import java.lang.reflect.*;

public final class FixedExceptionCovarianceHarness {
    private static int values, identities, states, failures;
    private interface Call { Object run(Object marker, String name, Carrier input) throws Throwable; }
    private static final class Leaf extends Core {
        public Core copy() throws First, Second {
            calls += 2;
            return super.copy();
        }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original unchecked covariance identity changed");
        identities++;
    }
    private static Method entry(Class<?> result) throws Exception {
        for (Method method : Core.class.getDeclaredMethods())
            if (method.getName().equals("copy") && method.getReturnType() == result) return method;
        throw new AssertionError("original typed or erased entry missing");
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static void supply(Core receiver, Object marker, String name, Carrier input) {
        if (receiver != null) { receiver.marker = marker; receiver.name = name; receiver.input = input; }
    }
    private static Object invoke(Method method, Object receiver, Object marker, String name,
                                 Carrier input) throws Throwable {
        supply((Core) receiver, marker, name, input);
        try { return method.invoke(receiver); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Call[] paths(Core receiver) throws Exception {
        Parent parent = receiver;
        Far far = receiver;
        Method typed = entry(Core.class), parentBridge = entry(Parent.class), objectBridge = entry(Object.class);
        Method required = Parent.class.getDeclaredMethod("copy");
        Method parentObject = null;
        for (Method method : Parent.class.getDeclaredMethods())
            if (method.getName().equals("copy") && method.getReturnType() == Object.class) parentObject = method;
        final Method originalParentBridge = parentObject;
        Method farRequired = Far.class.getDeclaredMethod("copy");
        return new Call[] {
            (marker, name, input) -> { supply(receiver, marker, name, input); return receiver.copy(); },
            (marker, name, input) -> { supply(receiver, marker, name, input); return parent.copy(); },
            (marker, name, input) -> { supply(receiver, marker, name, input); return far.copy(); },
            (marker, name, input) -> invoke(typed, receiver, marker, name, input),
            (marker, name, input) -> invoke(parentBridge, receiver, marker, name, input),
            (marker, name, input) -> invoke(objectBridge, receiver, marker, name, input),
            (marker, name, input) -> invoke(required, receiver, marker, name, input),
            (marker, name, input) -> invoke(originalParentBridge, receiver, marker, name, input),
            (marker, name, input) -> invoke(farRequired, receiver, marker, name, input)
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
        Method typed = entry(Core.class), bridge = entry(Parent.class), objectBridge = entry(Object.class);
        Method parentTyped = Parent.class.getDeclaredMethod("copy");
        if (Core.class.getTypeParameters().length != 0 || Core.class.getSuperclass() != Parent.class
                || !Core.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<java.lang.String>")
                || Parent.class.getTypeParameters().length != 1
                || !parentTyped.getGenericReturnType().getTypeName().equals("fixture.Parent<T>")
                || typed.getReturnType() != Core.class || typed.isBridge()
                || typed.getTypeParameters().length != 0 || bridge.getReturnType() != Parent.class
                || !bridge.isBridge() || !bridge.isSynthetic() || !objectBridge.isBridge()
                || !objectBridge.isSynthetic() || objectBridge.getExceptionTypes().length != 2
                || objectBridge.getExceptionTypes()[0] != First.class || objectBridge.getExceptionTypes()[1] != Second.class
                || bridge.getExceptionTypes().length != 2 || bridge.getExceptionTypes()[0] != First.class
                || bridge.getExceptionTypes()[1] != Second.class
                || typed.getExceptionTypes().length != 2 || typed.getExceptionTypes()[0] != First.class
                || typed.getExceptionTypes()[1] != Second.class
                || typed.getParameterTypes().length != 0)
            throw new AssertionError("original fixed Exceptions, complete superclass result or runtime bridges changed");
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
        System.out.println("fixed-exception-order=retained,complete-generic-parent-result=retained,original-bridge-dispatch=retained");
    }
}
