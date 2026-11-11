import fixture.*;
import java.lang.reflect.*;

public class GuardedReferenceBridgeHarness {
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
    @SuppressWarnings("rawtypes")
    public static void main(String[] args) throws Exception {
        Counter<Object> counter = new Counter<>();
        CountView<Object> typed = counter;
        CountView raw = counter;
        Element<Object> hit = new Element<>();
        Element<Object> miss = new Element<>();
        counter.expected = hit;
        for (Object wrong : new Object[]{null, new Object(), "wrong", 4}) {
            value(counter.contains(wrong), false);
            value(typed.contains(wrong), false);
        }
        check(counter.calls == 0);
        for (Element<Object> entry : new Element[]{hit, miss}) {
            value(counter.contains((Object) entry), entry == hit); same(counter.last, entry);
            value(typed.contains(entry), entry == hit); same(counter.last, entry);
            value(raw.contains(entry), entry == hit); same(counter.last, entry);
        }
        value(counter.contains(hit), true); same(counter.last, hit);
        value(counter.contains((Element<Object>) null), false); same(counter.last, null);
        check(counter.calls == 8);
        RuntimeException sentinel = new RuntimeException("owned failure");
        counter.failure = sentinel;
        value(typed.contains(null), false);
        value(counter.contains(new Object()), false); same(counter.last, null);
        fails(() -> counter.contains((Object) hit), sentinel); same(counter.last, hit);
        fails(() -> typed.contains(miss), sentinel); same(counter.last, miss);
        fails(() -> raw.contains(hit), sentinel); same(counter.last, hit);
        fails(() -> counter.contains((Element<Object>) null), sentinel); same(counter.last, null);
        check(counter.calls == 12);
        Child child = new Child();
        child.expected = hit;
        CountView<Object> api = child;
        value(api.contains(hit), true); same(child.last, hit);
        value(api.contains("wrong"), false); same(child.last, hit);
        check(child.calls == 1 && child.overrides == 1);
        child.failure = sentinel;
        fails(() -> api.contains(hit), sentinel); same(child.last, hit);
        check(child.calls == 2 && child.overrides == 2);
        Method bridge = Counter.class.getDeclaredMethod("contains", Object.class);
        check(bridge.isBridge() == args[0].equals("original"));
        check(!bridge.isSynthetic() && Modifier.isFinal(bridge.getModifiers()));
        Method target = Counter.class.getDeclaredMethod("contains", Element.class);
        ParameterizedType parameter = (ParameterizedType) target.getGenericParameterTypes()[0];
        check(parameter.getRawType() == Element.class
              && parameter.getActualTypeArguments()[0].equals(Counter.class.getTypeParameters()[0]));
        check(target.getGenericReturnType() == boolean.class);
        ParameterizedType edge = (ParameterizedType) Counter.class.getGenericInterfaces()[0];
        check(edge.getRawType() == CountView.class
              && edge.getActualTypeArguments()[0].equals(Counter.class.getTypeParameters()[0]));
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("guarded-reference-bridge=matched,virtual-override=retained,flags=reported");
    }
}
