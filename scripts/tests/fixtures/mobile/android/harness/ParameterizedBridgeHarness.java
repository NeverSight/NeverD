import fixture.ArrayReceiver;
import fixture.Box;
import fixture.ExactBox;
import fixture.Function;
import fixture.Nested;
import fixture.Receiver;
import fixture.Signal;
import fixture.Value;
import java.lang.reflect.Method;

public class ParameterizedBridgeHarness {
    static int identities;
    static int failures;
    static void check(boolean condition) {
        if (!condition) throw new AssertionError();
    }
    static void same(Object actual, Object expected) {
        check(actual == expected);
        ++identities;
    }
    static class Override extends Receiver {
        Object selected;
        Override(Object selected) { this.selected = selected; }
        public Object invoke(Box<? super Value> first, Box<? extends Signal> second) {
            return selected;
        }
    }
    static void bridge(Class<?> owner) {
        int bridges = 0;
        for (Method method : owner.getDeclaredMethods()) {
            if (method.isBridge()) {
                check(method.isSynthetic() && method.getReturnType() == Object.class);
                check(method.getParameterTypes()[0] == Object.class
                        && method.getParameterTypes()[1] == Object.class);
                ++bridges;
            }
        }
        check(bridges == 1);
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Object token = new Object();
        Value a = new Value(token);
        Value b = new Value(new Object());
        Signal s = new Signal(token);
        Signal t = new Signal(new Object());
        Box<Object> left = new Box<Object>(token);
        Box<Signal> right = new Box<Signal>(s);
        Receiver receiver = new Receiver();
        Function raw = receiver;
        for (Signal signal : new Signal[] {s, t, null, s}) {
            right.value = signal;
            receiver.value = signal == s ? a : b;
            int calls = receiver.calls;
            int writes = left.writes;
            int reads = right.reads;
            same(receiver.invoke(left, right), signal);
            same(raw.invoke(left, right), signal);
            same(left.value, receiver.value);
            same(receiver.first, left);
            same(receiver.second, right);
            check(receiver.calls == calls + 2 && left.writes == writes + 2 && right.reads == reads + 2);
        }
        receiver.failure = new IllegalStateException();
        RuntimeException sentinel = receiver.failure;
        for (Object invalid : new Object[] {token, "wrong", new Object[0]}) {
            int calls = receiver.calls;
            try {
                raw.invoke(invalid, right);
                throw new AssertionError();
            } catch (ClassCastException expected) { ++failures; }
            check(receiver.calls == calls);
            try {
                raw.invoke(left, invalid);
                throw new AssertionError();
            } catch (ClassCastException expected) { ++failures; }
            check(receiver.calls == calls);
        }
        for (Object[] values : new Object[][] {{left, right}, {null, right}, {left, null}, {null, null}}) {
            int calls = receiver.calls;
            int writes = left.writes;
            int reads = right.reads;
            try {
                raw.invoke(values[0], values[1]);
                throw new AssertionError();
            } catch (RuntimeException actual) {
                same(actual, sentinel);
                ++failures;
            }
            check(receiver.calls == calls + 1 && left.writes == writes && right.reads == reads);
        }
        receiver.failure = null;
        int writes = left.writes;
        int reads = right.reads;
        try {
            raw.invoke(null, right);
            throw new AssertionError();
        } catch (NullPointerException expected) { ++failures; }
        check(left.writes == writes && right.reads == reads);
        try {
            raw.invoke(left, null);
            throw new AssertionError();
        } catch (NullPointerException expected) { ++failures; }
        check(left.writes == writes + 1 && right.reads == reads);
        Box wrong = right;
        wrong.value = token;
        try {
            raw.invoke(left, right);
            throw new AssertionError();
        } catch (ClassCastException expected) { ++failures; }
        check(left.writes == writes + 2 && right.reads == reads + 1);
        right.value = t;
        raw = new Override(token);
        same(raw.invoke(left, right), token);
        check(((Receiver) raw).calls == 0);
        Nested<Value> nested = new ExactBox();
        same(nested.copy(left), left);
        same(nested.copy(null), null);
        Box<Value>[] values = (Box<Value>[]) new Box<?>[] {new Box<Value>(a)};
        Box<Signal>[] signals = (Box<Signal>[]) new Box<?>[] {right};
        ArrayReceiver arrays = new ArrayReceiver();
        raw = arrays;
        for (Signal signal : new Signal[] {s, t, null, s}) {
            right.value = signal;
            same(raw.invoke(values, signals), signal);
            same(arrays.first, values);
            same(arrays.second, signals);
        }
        arrays.failure = sentinel;
        for (Object invalid : new Object[] {token, new Object[] {left}, new Value[] {a}}) {
            int calls = arrays.calls;
            try {
                raw.invoke(invalid, signals);
                throw new AssertionError();
            } catch (ClassCastException expected) { ++failures; }
            check(arrays.calls == calls);
            try {
                raw.invoke(values, invalid);
                throw new AssertionError();
            } catch (ClassCastException expected) { ++failures; }
            check(arrays.calls == calls);
        }
        try {
            raw.invoke(values, signals);
            throw new AssertionError();
        } catch (RuntimeException actual) {
            same(actual, sentinel);
            ++failures;
        }
        arrays.failure = null;
        try {
            raw.invoke(values, new Box<?>[0]);
            throw new AssertionError();
        } catch (ArrayIndexOutOfBoundsException expected) { ++failures; }
        try {
            ((Function) null).invoke(left, right);
            throw new AssertionError();
        } catch (NullPointerException expected) { ++failures; }
        bridge(Receiver.class);
        bridge(ArrayReceiver.class);
        Method typed = Receiver.class.getDeclaredMethod("invoke", Box.class, Box.class);
        check(typed.getGenericParameterTypes()[0].getTypeName().equals("fixture.Box<? super fixture.Value>"));
        check(typed.getGenericParameterTypes()[1].getTypeName().equals("fixture.Box<? extends fixture.Signal>"));
        check(ExactBox.class.getDeclaredMethod("copy", Box.class).getGenericReturnType().getTypeName()
                .equals("fixture.Box<? super fixture.Value>"));
        check(ArrayReceiver.class.getDeclaredMethod("invoke", Box[].class, Box[].class)
                .getGenericParameterTypes()[0].getTypeName().equals("fixture.Box<fixture.Value>[]"));
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridges=2");
        System.out.println("full-generic-reflection=retained,mutable-effects=matched,cast-order=matched");
    }
}
