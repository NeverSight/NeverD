import fixture.*;
import java.lang.reflect.*;

public class InheritedErasedParametersHarness {
    private static int identities;
    private static int failures;
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    private static void same(Object actual, Object expected) { check(actual == expected); identities++; }
    private static void fails(Runnable action, RuntimeException exact) {
        try { action.run(); throw new AssertionError("missing failure"); }
        catch (RuntimeException error) { check(error == exact); failures++; }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Child<String, String> child = new Child<>();
        Root<String, String> root = child;
        Contract<String, String> typed = child;
        Contract raw = child;
        Object key = new Object();
        Object value = new Object();
        child.stored = value;
        check(!typed.remove(key, null));
        same(child.stored, value);
        same(child.lastKey, key);
        same(child.lastValue, null);
        check(raw.remove(key, value));
        same(child.stored, null);
        same(child.lastValue, value);
        check(root.remove(null, null));
        same(child.lastKey, null);
        check(child.calls == 3);
        RuntimeException sentinel = new RuntimeException("owned failure");
        child.failure = sentinel;
        child.stored = value;
        fails(() -> typed.remove(null, key), sentinel);
        fails(() -> raw.remove(value, null), sentinel);
        fails(() -> root.remove(key, value), sentinel);
        same(child.stored, value);
        same(child.lastKey, key);
        same(child.lastValue, value);
        check(child.calls == 6 && Trace.defaults == 0);
        child.failure = null;
        Object argument = new Object();
        Root rawRoot = child;
        same(rawRoot.get(argument), value);
        same(child.lastKey, argument);
        same(raw.get(argument), value);
        same(child.lastKey, argument);
        String text = new String("owned value");
        child.stored = text;
        same(typed.get(argument), text);
        same(child.get(argument), text);
        same(root.get("key"), text);
        check(rawRoot.contains(text) && raw.contains(text) && child.contains(text));
        same(child.lastKey, text);
        child.failure = sentinel;
        fails(() -> rawRoot.contains(argument), sentinel);
        fails(() -> raw.get(argument), sentinel);
        fails(() -> child.get(null), sentinel);
        same(child.lastKey, null);
        same(child.stored, text);
        check(child.rootCalls == 0 && child.overrideCalls == 11 && Trace.defaults == 0);
        child.failure = null;
        java.util.Collection<Object> items = java.util.Arrays.asList(argument, text);
        check(typed.containsAll(null));
        same(child.lastCollection, null);
        check(!raw.containsAll(items));
        same(child.lastCollection, items);
        check(!child.containsAll(items));
        same(child.lastCollection, items);
        child.failure = sentinel;
        fails(() -> typed.containsAll(items), sentinel);
        fails(() -> raw.containsAll(null), sentinel);
        fails(() -> child.containsAll(items), sentinel);
        same(child.lastCollection, items);
        check(child.collectionCalls == 6);
        ClosedView closed = new ClosedView();
        ClosedContract<Object> closedApi = closed;
        check(closedApi.addAll(null));
        same(closed.last, null);
        check(!closed.addAll(items));
        same(closed.last, items);
        check(!closedApi.addAll(items));
        same(closed.last, items);
        closed.failure = sentinel;
        fails(() -> closedApi.addAll(null), sentinel);
        same(closed.last, null);
        fails(() -> closed.addAll(items), sentinel);
        same(closed.last, items);
        fails(() -> closedApi.addAll(items), sentinel);
        same(closed.last, items);
        check(closed.calls == 6);
        Method method = child.getClass().getMethod("remove", Object.class, Object.class);
        check(method.getDeclaringClass() == Root.class && !method.isBridge());
        Type[] parameters = method.getGenericParameterTypes();
        check(parameters[0].equals(Root.class.getTypeParameters()[0]));
        check(parameters[1].equals(Root.class.getTypeParameters()[1]));
        Method inherited = Root.class.getDeclaredMethod("get", Object.class);
        Method override = Child.class.getDeclaredMethod("get", Object.class);
        check(inherited.getGenericParameterTypes()[0].equals(Root.class.getTypeParameters()[0]));
        check(inherited.getGenericReturnType().equals(Root.class.getTypeParameters()[1]));
        check(override.getGenericParameterTypes()[0] == Object.class);
        check(override.getGenericReturnType().equals(Child.class.getTypeParameters()[1]));
        Type input = ((ParameterizedType) Child.class.getDeclaredMethod(
            "containsAll", java.util.Collection.class).getGenericParameterTypes()[0])
            .getActualTypeArguments()[0];
        Type contractInput = ((ParameterizedType) Contract.class.getDeclaredMethod(
            "containsAll", java.util.Collection.class).getGenericParameterTypes()[0])
            .getActualTypeArguments()[0];
        check(input.equals(contractInput));
        check(((WildcardType) input).getUpperBounds()[0] == Object.class
              && ((WildcardType) input).getLowerBounds().length == 0);
        ParameterizedType closedEdge = (ParameterizedType) ClosedView.class.getGenericInterfaces()[0];
        check(closedEdge.getRawType() == ClosedContract.class
              && closedEdge.getActualTypeArguments()[0] == Object.class);
        Type closedInput = ((ParameterizedType) ClosedView.class.getDeclaredMethod(
            "addAll", java.util.Collection.class).getGenericParameterTypes()[0]).getActualTypeArguments()[0];
        check(closedInput.equals(input));
        ParameterizedType base = (ParameterizedType) Child.class.getGenericSuperclass();
        ParameterizedType api = (ParameterizedType) Child.class.getGenericInterfaces()[0];
        check(base.getRawType() == Root.class && api.getRawType() == Contract.class);
        for (int i = 0; i < 2; ++i) {
            check(base.getActualTypeArguments()[i].equals(Child.class.getTypeParameters()[i]));
            check(api.getActualTypeArguments()[i].equals(Child.class.getTypeParameters()[i]));
        }
        for (Class<?> owner : new Class<?>[]{Root.class, Child.class})
            for (Method declared : owner.getDeclaredMethods()) check(!declared.isBridge());
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridges=0");
        System.out.println("inherited-erased-parameters=matched,default-effects=absent");
    }
}
