import fixture.*;
import java.lang.reflect.*;

public class GuardedDefaultSuperAdapterHarness {
    public static class Child extends Counter<Object> {
        public int overrides;
        @Override public Holder<Object> getOrDefault(Element<Object> key, Holder<Object> fallback) {
            overrides++;
            return super.getOrDefault(key, fallback);
        }
    }
    private static int values, identities, failures;
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    private static void same(Object actual, Object expected) { check(actual == expected); identities++; }
    private static void count(int actual, int expected) { check(actual == expected); values++; }
    private static void badCast(Runnable action) {
        try { action.run(); throw new AssertionError("missing ordered cast failure"); }
        catch (ClassCastException expected) { failures++; }
    }
    private static void fails(Runnable action, RuntimeException sentinel) {
        try { action.run(); throw new AssertionError("missing original callee failure"); }
        catch (RuntimeException error) { same(error, sentinel); failures++; }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Counter<Object> counter = new Counter<>();
        CountView<Holder<Object>> api = counter;
        CountView raw = counter;
        Root<Holder<Object>> root = counter;
        Middle<Holder<Object>> middle = counter;
        Element<Object> key = new Element<>();
        Holder<Object> holder = new Holder<>(), fallback = new Holder<>();
        Object wrongDefault = new Object();
        counter.backing = holder;
        for (Object wrongKey : new Object[]{null, new Object(), "wrong", 4}) {
            same(counter.getOrDefault(wrongKey, fallback), fallback);
            same(api.getOrDefault(wrongKey, fallback), fallback);
            same(raw.getOrDefault(wrongKey, wrongDefault), wrongDefault);
            same(((CountView) root).getOrDefault(wrongKey, wrongDefault), wrongDefault);
            same(raw.getOrDefault(wrongKey, null), null);
        }
        count(counter.calls, 0); same(counter.last, null);
        same(counter.getOrDefault(key, fallback), holder); same(counter.last, key);
        same(counter.getOrDefault((Object) key, fallback), holder); same(counter.last, key);
        same(api.getOrDefault(key, fallback), holder); same(counter.last, key);
        same(raw.getOrDefault(key, fallback), holder); same(counter.last, key);
        same(root.getOrDefault(key, fallback), holder); same(counter.last, key);
        same(middle.getOrDefault(key, fallback), holder); same(counter.last, key);
        same(counter.getOrDefault((Element<Object>) null, fallback), holder); same(counter.last, null);
        count(counter.calls, 7);
        badCast(() -> raw.getOrDefault(key, wrongDefault)); same(counter.last, null);
        count(counter.calls, 7);
        counter.backing = null;
        same(counter.getOrDefault(key, fallback), fallback); same(counter.last, key);
        same(api.getOrDefault(key, fallback), fallback); same(counter.last, key);
        same(raw.getOrDefault(key, null), null); same(counter.last, key);
        count(counter.calls, 10);
        counter.backing = new Object();
        badCast(() -> counter.getOrDefault(key, fallback)); same(counter.last, key);
        badCast(() -> api.getOrDefault(key, fallback)); same(counter.last, key);
        badCast(() -> raw.getOrDefault(key, fallback)); same(counter.last, key);
        badCast(() -> root.getOrDefault(key, fallback)); same(counter.last, key);
        count(counter.calls, 14);
        RuntimeException sentinel = new IllegalStateException("owned failure");
        counter.failure = sentinel;
        same(raw.getOrDefault("wrong", wrongDefault), wrongDefault); same(counter.last, key);
        count(counter.calls, 14);
        badCast(() -> raw.getOrDefault(key, wrongDefault)); same(counter.last, key);
        count(counter.calls, 14);
        fails(() -> counter.getOrDefault(key, fallback), sentinel); same(counter.last, key);
        fails(() -> counter.getOrDefault((Object) key, fallback), sentinel); same(counter.last, key);
        fails(() -> api.getOrDefault(key, fallback), sentinel); same(counter.last, key);
        fails(() -> raw.getOrDefault(key, fallback), sentinel); same(counter.last, key);
        fails(() -> root.getOrDefault(key, fallback), sentinel); same(counter.last, key);
        fails(() -> middle.getOrDefault(key, fallback), sentinel); same(counter.last, key);
        fails(() -> counter.getOrDefault((Element<Object>) null, fallback), sentinel); same(counter.last, null);
        count(counter.calls, 21);
        counter.failure = null; counter.backing = holder;
        api.forEach(value -> same(value, holder)); same(counter.last, null);
        count(counter.calls, 22);
        Child child = new Child(); child.backing = holder;
        CountView childRaw = child;
        same(childRaw.getOrDefault("wrong", wrongDefault), wrongDefault);
        count(child.calls, 0); count(child.overrides, 0);
        same(childRaw.getOrDefault(key, fallback), holder); same(child.last, key);
        same(child.getOrDefault(key, fallback), holder); same(child.last, key);
        count(child.calls, 2); count(child.overrides, 2);
        badCast(() -> childRaw.getOrDefault(key, wrongDefault)); same(child.last, key);
        count(child.calls, 2); count(child.overrides, 2);
        child.failure = sentinel;
        fails(() -> childRaw.getOrDefault(key, fallback), sentinel); same(child.last, key);
        count(child.calls, 3); count(child.overrides, 3);
        boolean original = args[0].equals("original");
        check(Counter.class.getGenericSuperclass().getTypeName().equals(original
            ? "fixture.Middle<fixture.Holder<T>>" : "fixture.Middle"));
        check(Counter.class.getGenericInterfaces()[0].getTypeName().equals(original
            ? "fixture.CountView<fixture.Holder<T>>" : "fixture.CountView"));
        check(Middle.class.getGenericSuperclass().getTypeName().equals("fixture.Root<M>"));
        check(Root.class.getGenericInterfaces()[0].getTypeName().equals("fixture.CountView<R>"));
        int declared = 0;
        for (Method method : Counter.class.getDeclaredMethods()) {
            if (method.getName().equals("__neverdThrow")) {
                check(Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers()));
                check(method.getReturnType() == RuntimeException.class && method.getParameterCount() == 1
                    && method.getParameterTypes()[0] == Throwable.class);
                continue;
            }
            declared++;
            check(method.getName().equals("getOrDefault") && method.getParameterCount() == 2);
            boolean erased = method.getReturnType() == Object.class;
            check(method.isSynthetic() == (erased && original));
            check(method.isBridge() == original);
            check(Modifier.isFinal(method.getModifiers()) == (method.getParameterTypes()[0] == Object.class));
            if (!erased) {
                check(method.getGenericReturnType().getTypeName().equals("fixture.Holder<T>"));
                check(method.getGenericParameterTypes()[0].getTypeName().equals(
                    method.getParameterTypes()[0] == Object.class ? "java.lang.Object" : "fixture.Element<T>"));
                check(method.getGenericParameterTypes()[1].getTypeName().equals("fixture.Holder<T>"));
            }
        }
        check(declared == 3);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("negative-default=unchanged,late-cast-and-super-default=matched,metadata-differences=reported");
    }
}
