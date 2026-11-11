import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public final class TerminalThrowBridgeHarness {
    private static int identities, values, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("identity changed");
        identities++;
    }
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("value changed");
        values++;
    }
    private static void throwsSame(Runnable operation) {
        try { operation.run(); throw new AssertionError("missing owned exception"); }
        catch (UnsupportedOperationException e) {
            if (!e.getMessage().equals("owned immutable operation"))
                throw new AssertionError("exception message changed");
            failures++;
        }
    }
    public static void main(String[] args) throws Exception {
        Counter<String,Integer> counter = new Counter<>();
        CountView raw = counter;
        Element<String,Integer> element = new Element<>();
        element.key = "owned value";
        element.value = 11;
        throwsSame(() -> counter.add(element));
        throwsSame(() -> counter.add((Element<String,Integer>) null));
        for (Object argument : new Object[] {element, new Object(), "incompatible", null}) {
            throwsSame(() -> counter.add(argument));
            throwsSame(() -> raw.add(argument));
        }
        List<Element<String,Integer>> rows = new ArrayList<>();
        rows.add(element);
        counter.value = rows.iterator();
        identity(counter.iterator(), counter.value);
        identity(raw.iterator(), counter.value);
        identity(raw.iterator().next(), element);
        value(counter.calls, 3);
        value(element.value, 11);
        counter.value = null;
        identity(counter.iterator(), null);
        identity(raw.iterator(), null);
        value(counter.calls, 5);
        counter.failure = new IllegalStateException("owned iterator failure");
        for (int i = 0; i < 2; ++i) {
            try {
                if (i == 0) counter.iterator(); else raw.iterator();
                throw new AssertionError("iterator exception missing");
            } catch (IllegalStateException e) { identity(e, counter.failure); failures++; }
            value(counter.calls, 6 + i);
        }
        counter.failure = null;
        counter.value = rows.iterator();
        Iterator iterator = raw.iterator();
        identity(iterator.next(), element);
        iterator.remove();
        value(rows.size(), 0);
        value(counter.calls, 8);
        Method bridge = Counter.class.getMethod("add", Object.class);
        Method typed = Counter.class.getMethod("add", Element.class);
        boolean recovered = args[0].equals("recovered");
        if (bridge.isBridge() == recovered || bridge.isSynthetic() == recovered
                || Modifier.isFinal(bridge.getModifiers()))
            throw new AssertionError("bridge flags differ from receipt");
        Type api = Counter.class.getGenericInterfaces()[0];
        if (recovered) {
            if (!api.equals(CountView.class)) throw new AssertionError("raw interface projection lost");
        } else if (!(api instanceof ParameterizedType)) throw new AssertionError("original interface missing");
        Type[] variables = Counter.class.getTypeParameters();
        ParameterizedType parameter = (ParameterizedType) typed.getGenericParameterTypes()[0];
        ParameterizedType result = (ParameterizedType) Counter.class.getMethod("iterator").getGenericReturnType();
        ParameterizedType nested = (ParameterizedType) result.getActualTypeArguments()[0];
        ParameterizedType field = (ParameterizedType) Counter.class.getField("value").getGenericType();
        ParameterizedType fieldElement = (ParameterizedType) field.getActualTypeArguments()[0];
        for (int i = 0; i < 2; ++i) {
            Type expected = parameter.getActualTypeArguments()[i];
            if (!(expected instanceof WildcardType) ||
                    !((WildcardType) expected).getUpperBounds()[0].equals(variables[i]))
                throw new AssertionError("typed parameter changed");
            Type actual = nested.getActualTypeArguments()[i];
            if (!(recovered ? expected.equals(actual) : variables[i].equals(actual)))
                throw new AssertionError("iterator generic receipt changed");
            if (!fieldElement.getActualTypeArguments()[i].equals(variables[i]))
                throw new AssertionError("original field reflection changed");
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("terminal-throw=matched,unchecked-arguments=retained,metadata-difference=reported");
    }
}
