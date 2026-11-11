import fixture.*;
import java.lang.reflect.*;

public class SuperclassScalarAdapterHarness {
    public static class Child extends Counter<Object> {
        public int overrides;
        @Override public boolean contains(Element<Object> value) {
            overrides++;
            return super.contains(value);
        }
    }
    private static int values;
    private static int identities;
    private static int failures;
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    private static void value(boolean actual, boolean expected) { check(actual == expected); values++; }
    private static void same(Object actual, Object expected) { check(actual == expected); identities++; }
    private static void fails(Runnable action, RuntimeException sentinel) {
        try { action.run(); throw new AssertionError("missing failure"); }
        catch (RuntimeException error) { check(error == sentinel); failures++; }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Counter<Object> counter = new Counter<>();
        CountView<Object> api = counter;
        CountView raw = counter;
        Root<Object,String> root = counter;
        Middle<Object,String> middle = counter;
        Element<Object> hit = new Element<>();
        Element<Object> miss = new Element<>();
        counter.expected = hit;
        for (Object wrong : new Object[]{null, new Object(), "wrong", 4}) {
            value(counter.contains(wrong), false);
            value(api.contains(wrong), false);
            value(root.contains(wrong), false);
            value(middle.contains(wrong), false);
        }
        check(counter.calls == 0); same(counter.last, null);
        for (Element<Object> entry : new Element[]{hit, miss}) {
            value(counter.contains(entry), entry == hit); same(counter.last, entry);
            value(counter.contains((Object) entry), entry == hit); same(counter.last, entry);
            value(api.contains(entry), entry == hit); same(counter.last, entry);
            value(raw.contains(entry), entry == hit); same(counter.last, entry);
            value(root.contains(entry), entry == hit); same(counter.last, entry);
            value(middle.contains(entry), entry == hit); same(counter.last, entry);
        }
        value(counter.contains((Element<Object>) null), false); same(counter.last, null);
        check(counter.calls == 13);
        counter.expected = null;
        value(counter.contains((Element<Object>) null), true); same(counter.last, null);
        value(counter.contains((Object) null), false); same(counter.last, null);
        check(counter.calls == 14);
        counter.expected = hit;
        RuntimeException sentinel = new RuntimeException("owned failure");
        counter.failure = sentinel;
        value(api.contains(null), false);
        value(root.contains(new Object()), false); same(counter.last, null);
        fails(() -> counter.contains(hit), sentinel); same(counter.last, hit);
        fails(() -> counter.contains((Object) hit), sentinel); same(counter.last, hit);
        fails(() -> api.contains(miss), sentinel); same(counter.last, miss);
        fails(() -> raw.contains(hit), sentinel); same(counter.last, hit);
        fails(() -> root.contains(miss), sentinel); same(counter.last, miss);
        fails(() -> middle.contains(hit), sentinel); same(counter.last, hit);
        fails(() -> counter.contains((Element<Object>) null), sentinel); same(counter.last, null);
        check(counter.calls == 21);
        Child child = new Child();
        child.expected = hit;
        Root<Object,String> childRoot = child;
        CountView<Object> childApi = child;
        value(childApi.contains(hit), true); same(child.last, hit);
        value(childRoot.contains(miss), false); same(child.last, miss);
        value(child.contains(hit), true); same(child.last, hit);
        value(childApi.contains("wrong"), false); same(child.last, hit);
        check(child.calls == 3 && child.overrides == 3);
        child.failure = sentinel;
        fails(() -> childRoot.contains(hit), sentinel); same(child.last, hit);
        fails(() -> child.contains((Element<Object>) null), sentinel); same(child.last, null);
        check(child.calls == 5 && child.overrides == 5);
        for (Class<?> parameter : new Class<?>[]{Object.class, Element.class}) {
            Method bridge = Counter.class.getDeclaredMethod("contains", parameter);
            check(bridge.isBridge() == args[0].equals("original"));
            check(!bridge.isSynthetic());
            check(Modifier.isFinal(bridge.getModifiers()) == (parameter == Object.class));
            check(bridge.getGenericReturnType() == boolean.class);
        }
        Method target = Counter.class.getDeclaredMethod("contains", Element.class);
        ParameterizedType parameter = (ParameterizedType) target.getGenericParameterTypes()[0];
        check(parameter.getRawType() == Element.class
              && parameter.getActualTypeArguments()[0].equals(Counter.class.getTypeParameters()[0]));
        check(Counter.class.getGenericSuperclass().getTypeName().equals("fixture.Middle<T, java.lang.String>"));
        check(Middle.class.getGenericSuperclass().getTypeName().equals("fixture.Root<M, N>"));
        check(Root.class.getGenericInterfaces()[0].getTypeName().equals("fixture.CountView<R>"));
        check(Counter.class.getGenericInterfaces()[0].getTypeName().equals("fixture.CountView<T>"));
        Method inherited = Root.class.getDeclaredMethod("contains", Object.class);
        check(inherited.getGenericParameterTypes()[0] == Object.class);
        check(inherited.getGenericReturnType() == boolean.class && !inherited.isBridge());
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("scalar-superclass-adapters=matched,virtual-overrides-and-super-effects=retained,flags=reported");
    }
}
