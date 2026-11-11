import fixture.*;
import java.lang.reflect.*;

public class GuardedIntegerBridgeHarness {
    public static class Child extends Counter<Object> {
        public int overrides;
        @Override public int indexOf(Element<Object> value) {
            overrides++;
            return super.indexOf(value);
        }
    }
    private static int values;
    private static int identities;
    private static int failures;
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    private static void value(int actual, int expected) { check(actual == expected); values++; }
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
        counter.result = 71;
        for (Object wrong : new Object[]{null, new Object(), "wrong", 4}) {
            value(counter.indexOf(wrong), -1);
            value(typed.indexOf(wrong), -1);
        }
        check(counter.calls == 0);
        for (Element<Object> entry : new Element[]{hit, miss}) {
            value(counter.indexOf((Object) entry), entry == hit ? 71 : -9); same(counter.last, entry);
            value(typed.indexOf(entry), entry == hit ? 71 : -9); same(counter.last, entry);
            value(raw.indexOf(entry), entry == hit ? 71 : -9); same(counter.last, entry);
        }
        value(counter.indexOf(hit), 71); same(counter.last, hit);
        value(counter.indexOf((Element<Object>) null), -9); same(counter.last, null);
        check(counter.calls == 8);
        RuntimeException sentinel = new RuntimeException("owned failure");
        counter.failure = sentinel;
        value(typed.indexOf(null), -1);
        value(counter.indexOf(new Object()), -1); same(counter.last, null);
        fails(() -> counter.indexOf((Object) hit), sentinel); same(counter.last, hit);
        fails(() -> typed.indexOf(miss), sentinel); same(counter.last, miss);
        fails(() -> raw.indexOf(hit), sentinel); same(counter.last, hit);
        fails(() -> counter.indexOf((Element<Object>) null), sentinel); same(counter.last, null);
        check(counter.calls == 12);
        counter.failure = null;
        for (int number : new int[] {Integer.MIN_VALUE, -1, 0, 1, Integer.MAX_VALUE}) {
            counter.result = number;
            value(counter.indexOf((Object) hit), number); same(counter.last, hit);
            value(typed.indexOf(hit), number); same(counter.last, hit);
            value(raw.indexOf(hit), number); same(counter.last, hit);
        }
        check(counter.calls == 27);
        Child child = new Child();
        child.expected = hit;
        child.result = 71;
        CountView<Object> api = child;
        value(api.indexOf(hit), 71); same(child.last, hit);
        value(api.indexOf("wrong"), -1); same(child.last, hit);
        check(child.calls == 1 && child.overrides == 1);
        child.failure = sentinel;
        fails(() -> api.indexOf(hit), sentinel); same(child.last, hit);
        check(child.calls == 2 && child.overrides == 2);
        Method bridge = Counter.class.getDeclaredMethod("indexOf", Object.class);
        check(bridge.isBridge() == args[0].equals("original"));
        check(!bridge.isSynthetic() && Modifier.isFinal(bridge.getModifiers()));
        Method target = Counter.class.getDeclaredMethod("indexOf", Element.class);
        ParameterizedType parameter = (ParameterizedType) target.getGenericParameterTypes()[0];
        check(parameter.getRawType() == Element.class
              && parameter.getActualTypeArguments()[0].equals(Counter.class.getTypeParameters()[0]));
        check(target.getGenericReturnType() == int.class);
        ParameterizedType edge = (ParameterizedType) Counter.class.getGenericInterfaces()[0];
        check(edge.getRawType() == CountView.class
              && edge.getActualTypeArguments()[0].equals(Counter.class.getTypeParameters()[0]));
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("guarded-integer-bridge=matched,virtual-override=retained,flags=reported");
    }
}
