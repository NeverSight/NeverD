import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public class IndexedReferenceBridgeHarness {
    private static int values, identities, failures;
    private static void check(boolean condition) {
        if (!condition) throw new AssertionError("owned indexed bridge mismatch");
    }
    private static void value(int actual, int expected) { check(actual == expected); values++; }
    private static void same(Object actual, Object expected) { check(actual == expected); identities++; }
    private static void fails(Runnable action, Class<?> type, RuntimeException sentinel) {
        try { action.run(); throw new AssertionError("missing indexed failure"); }
        catch (RuntimeException error) {
            check(type.isInstance(error));
            if (sentinel != null) same(error, sentinel);
            failures++;
        }
    }
    @SuppressWarnings("rawtypes")
    public static void main(String[] args) throws Exception {
        Object a = new Object(), b = new Object(), c = new Object();
        Counter<Object> counter = new Counter<>();
        counter.values = new ArrayList<>(Arrays.asList(a, b, c));
        CountView<Object> typed = counter;
        CountView raw = counter;
        same(counter.remove(1), b);
        value(counter.calls, 1);
        value(counter.lastIndex, 1);
        value(counter.values.size(), 2);
        same(typed.remove(0), a);
        same(raw.remove(0), c);
        value(counter.calls, 3);
        value(counter.values.size(), 0);
        for (int index : new int[]{-1, 0, Integer.MIN_VALUE, Integer.MAX_VALUE})
            fails(() -> typed.remove(index), IndexOutOfBoundsException.class, null);
        value(counter.calls, 7);
        value(counter.lastIndex, Integer.MAX_VALUE);
        counter.values = null;
        fails(() -> typed.remove(0), NullPointerException.class, null);
        value(counter.calls, 8);
        RuntimeException sentinel = new IllegalStateException("owned indexed failure");
        counter.failure = sentinel;
        fails(() -> counter.remove(4), IllegalStateException.class, sentinel);
        fails(() -> typed.remove(5), IllegalStateException.class, sentinel);
        fails(() -> raw.remove(6), IllegalStateException.class, sentinel);
        value(counter.calls, 11);
        Counter<Object> absent = null;
        fails(() -> absent.remove(7), NullPointerException.class, null);
        value(counter.calls, 11);
        Child<Object> child = new Child<>();
        child.values = new ArrayList<>(Arrays.asList(a, b));
        CountView<Object> childApi = child;
        same(child.remove(0), a);
        same(childApi.remove(0), b);
        value(child.overrides, 2);
        value(child.calls, 2);
        value(child.values.size(), 0);
        child.failure = sentinel;
        fails(() -> childApi.remove(7), IllegalStateException.class, sentinel);
        value(child.overrides, 3);
        value(child.calls, 3);
        value(child.lastIndex, 7);
        Method bridge = Counter.class.getDeclaredMethod("remove", int.class);
        Method target = Counter.class.getDeclaredMethod("removeAt", int.class);
        check(bridge.isBridge() == args[0].equals("original") && !bridge.isSynthetic());
        check(Modifier.isFinal(bridge.getModifiers()) && bridge.getReturnType() == Object.class);
        check(bridge.getGenericReturnType().equals(Counter.class.getTypeParameters()[0]));
        check(bridge.getGenericReturnType().equals(target.getGenericReturnType()));
        check(bridge.getGenericParameterTypes()[0] == int.class);
        check(Child.class.getMethod("remove", int.class).getDeclaringClass() == Counter.class);
        for (Method own : Child.class.getDeclaredMethods())
            check(!own.isBridge() && !own.getName().equals("remove"));
        Type variable = Child.class.getTypeParameters()[0];
        ParameterizedType parent = (ParameterizedType) Child.class.getGenericSuperclass();
        ParameterizedType api = (ParameterizedType) Child.class.getGenericInterfaces()[0];
        check(parent.getActualTypeArguments()[0].equals(variable));
        check(api.getActualTypeArguments()[0].equals(variable));
        check(Child.class.getDeclaredMethod("removeAt", int.class).getGenericReturnType().equals(variable));
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("indexed-reference-bridge=matched,index-and-virtual-override=retained,flags=reported");
    }
}
