import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public final class GenericReferenceGetterHarness {
    private static int identities, values, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("identity changed");
        identities++;
    }
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("value changed");
        values++;
    }
    public static void main(String[] args) throws Exception {
        Counter<String> counter = new Counter<>();
        CountView<String> typed = counter;
        CountView raw = counter;
        Set<String> first = new HashSet<>();
        first.add("owned");
        counter.values = first;
        identity(counter.items(), first);
        identity(typed.items(), first);
        identity(raw.items(), first);
        identity(counter.last, first);
        value(counter.calls, 3);
        typed.items().add("mutable");
        value(first.size(), 2);
        value(counter.calls, 4);
        Set<String> second = new HashSet<>();
        counter.values = second;
        identity(typed.items(), second);
        identity(counter.last, second);
        counter.values = null;
        identity(counter.items(), null);
        identity(raw.items(), null);
        identity(counter.last, null);
        value(counter.calls, 7);
        RuntimeException problem = new IllegalStateException("owned getter failure");
        counter.failure = problem;
        for (int i = 0; i < 2; ++i) {
            try {
                if (i == 0) counter.items(); else typed.items();
                throw new AssertionError("getter failure missing");
            } catch (IllegalStateException e) { identity(e, problem); failures++; }
            value(counter.calls, 8 + i);
        }
        Child<String> derived = new Child<>();
        derived.values = first;
        derived.alternate = second;
        CountView<String> derivedView = derived;
        identity(derived.items(), second);
        identity(derivedView.items(), second);
        identity(derived.last, first);
        value(derived.calls, 2);
        value(derived.overrides, 2);
        derived.failure = problem;
        try { derivedView.items(); throw new AssertionError("override failure missing"); }
        catch (IllegalStateException e) { identity(e, problem); failures++; }
        value(derived.calls, 3);
        value(derived.overrides, 3);
        Counter<String> absent = null;
        try { absent.items(); throw new AssertionError("receiver failure missing"); }
        catch (NullPointerException e) { failures++; }
        Method bridge = Counter.class.getMethod("items");
        Method target = Counter.class.getMethod("getItems");
        boolean recovered = args[0].equals("recovered");
        if (bridge.isBridge() == recovered || bridge.isSynthetic()
                || !Modifier.isFinal(bridge.getModifiers()))
            throw new AssertionError("bridge flag receipt changed");
        if (!bridge.getGenericReturnType().equals(target.getGenericReturnType()))
            throw new AssertionError("full generic getter type changed");
        Type variable = Counter.class.getTypeParameters()[0];
        ParameterizedType result = (ParameterizedType) bridge.getGenericReturnType();
        if (!result.getActualTypeArguments()[0].equals(variable))
            throw new AssertionError("return scope changed");
        ParameterizedType api = (ParameterizedType) Counter.class.getGenericInterfaces()[0];
        if (!api.getActualTypeArguments()[0].equals(variable))
            throw new AssertionError("interface scope changed");
        if (Child.class.getMethod("items").getDeclaringClass() != Counter.class)
            throw new AssertionError("inherited final bridge was duplicated");
        for (Method own : Child.class.getDeclaredMethods())
            if (own.getName().equals("items") || own.isBridge())
                throw new AssertionError("child gained a bridge");
        Type childVariable = Child.class.getTypeParameters()[0];
        ParameterizedType childParent = (ParameterizedType) Child.class.getGenericSuperclass();
        ParameterizedType childApi = (ParameterizedType) Child.class.getGenericInterfaces()[0];
        ParameterizedType childGetter = (ParameterizedType) Child.class.getDeclaredMethod("getItems").getGenericReturnType();
        if (!childParent.getActualTypeArguments()[0].equals(childVariable)
                || !childApi.getActualTypeArguments()[0].equals(childVariable)
                || !childGetter.getActualTypeArguments()[0].equals(childVariable))
            throw new AssertionError("inherited getter substitution changed");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("generic-reference-getter=matched,virtual-override=retained,flags=reported");
    }
}
