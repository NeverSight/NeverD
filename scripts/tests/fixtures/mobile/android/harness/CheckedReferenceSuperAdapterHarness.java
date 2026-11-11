import fixture.*;
import java.lang.reflect.*;

public class CheckedReferenceSuperAdapterHarness {
    public static class Child extends Counter<Object> {
        public int overrides;
        @Override public Holder<Object> get(Element<Object> value) {
            overrides++;
            return super.get(value);
        }
    }
    private static int values, identities, failures;
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    private static void same(Object actual, Object expected) { check(actual == expected); identities++; }
    private static void count(int actual, int expected) { check(actual == expected); values++; }
    private static void fails(Runnable action, RuntimeException sentinel) {
        try { action.run(); throw new AssertionError("missing failure"); }
        catch (RuntimeException error) { same(error, sentinel); failures++; }
    }
    private static void badCast(Runnable action) {
        try { action.run(); throw new AssertionError("missing checked result"); }
        catch (ClassCastException expected) { failures++; }
    }
    @SuppressWarnings("rawtypes")
    public static void main(String[] args) throws Exception {
        Counter<Object> counter = new Counter<>();
        CountView<Holder<Object>> api = counter;
        CountView raw = counter;
        Root<Holder<Object>> root = counter;
        Middle<Holder<Object>> middle = counter;
        Element<Object> element = new Element<>();
        Holder<Object> holder = new Holder<>();
        holder.payload = new Object();
        counter.backing = holder;
        for (Object wrong : new Object[]{null, new Object(), "wrong", 4}) {
            same(counter.get(wrong), null); same(api.get(wrong), null);
            same(raw.get(wrong), null); same(root.get(wrong), null);
        }
        count(counter.calls, 0); same(counter.last, null);
        same(counter.get(element), holder); same(counter.last, element);
        same(counter.get((Object) element), holder); same(counter.last, element);
        same(api.get(element), holder); same(counter.last, element);
        same(raw.get(element), holder); same(counter.last, element);
        same(root.get(element), holder); same(counter.last, element);
        same(middle.get(element), holder); same(counter.last, element);
        same(counter.get((Element<Object>) null), holder); same(counter.last, null);
        count(counter.calls, 7);
        Object payload = holder.payload;
        same(((Holder<?>) raw.get(element)).payload, payload); same(counter.last, element);
        count(counter.calls, 8);
        counter.backing = null;
        same(counter.get(element), null); same(api.get(element), null); same(raw.get(element), null);
        count(counter.calls, 11); same(counter.last, element);
        counter.backing = new Object();
        badCast(() -> counter.get(element)); same(counter.last, element);
        badCast(() -> api.get(element)); same(counter.last, element);
        badCast(() -> raw.get(element)); same(counter.last, element);
        badCast(() -> root.get(element)); same(counter.last, element);
        count(counter.calls, 15);
        same(root.get("wrong"), null); same(counter.last, element);
        count(counter.calls, 15);
        RuntimeException sentinel = new IllegalStateException("owned failure");
        counter.failure = sentinel;
        same(api.get(null), null); same(counter.last, element); count(counter.calls, 15);
        fails(() -> counter.get(element), sentinel); same(counter.last, element);
        fails(() -> counter.get((Object) element), sentinel); same(counter.last, element);
        fails(() -> api.get(element), sentinel); same(counter.last, element);
        fails(() -> raw.get(element), sentinel); same(counter.last, element);
        fails(() -> root.get(element), sentinel); same(counter.last, element);
        fails(() -> middle.get(element), sentinel); same(counter.last, element);
        fails(() -> counter.get((Element<Object>) null), sentinel); same(counter.last, null);
        count(counter.calls, 22);
        Child child = new Child();
        child.backing = holder;
        CountView<Holder<Object>> childApi = child;
        Root<Holder<Object>> childRoot = child;
        same(childApi.get(element), holder); same(child.last, element);
        same(childRoot.get(element), holder); same(child.last, element);
        same(child.get(element), holder); same(child.last, element);
        count(child.calls, 3); count(child.overrides, 3);
        same(childApi.get("wrong"), null); same(child.last, element);
        count(child.calls, 3); count(child.overrides, 3);
        child.failure = sentinel;
        fails(() -> childRoot.get(element), sentinel); same(child.last, element);
        count(child.calls, 4); count(child.overrides, 4);
        check(Counter.class.getGenericSuperclass().getTypeName().equals("fixture.Middle<fixture.Holder<T>>"));
        check(Middle.class.getGenericSuperclass().getTypeName().equals("fixture.Root<M>"));
        check(Root.class.getGenericInterfaces()[0].getTypeName().equals("fixture.CountView<R>"));
        check(Counter.class.getGenericInterfaces()[0].getTypeName().equals("fixture.CountView<fixture.Holder<T>>"));
        int declared = 0;
        for (Method method : Counter.class.getDeclaredMethods()) {
            if (method.getName().equals("__neverdThrow")) {
                check(Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers()));
                check(method.getReturnType() == RuntimeException.class && method.getParameterCount() == 1
                    && method.getParameterTypes()[0] == Throwable.class);
                continue;
            }
            declared++;
            check(method.getName().equals("get") && method.getParameterCount() == 1);
            boolean erased = method.getReturnType() == Object.class;
            check(method.isSynthetic() == erased);
            check(method.isBridge() == (erased || args[0].equals("original")));
            check(Modifier.isFinal(method.getModifiers()) == (erased
                ? args[0].equals("original") : method.getParameterTypes()[0] == Object.class));
            if (!erased) {
                check(method.getGenericReturnType().getTypeName().equals("fixture.Holder<T>"));
                check(method.getGenericParameterTypes()[0].getTypeName().equals(
                    method.getParameterTypes()[0] == Object.class ? "java.lang.Object" : "fixture.Element<T>"));
            }
        }
        check(declared == 3);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("checked-super-results=retained,guards-and-virtual-dispatch=matched,metadata-differences=reported");
    }
}
