import fixture.*;
import java.lang.reflect.*;

public class PlainIntegerBridgeHarness {
    private static int cases;
    private static int failures;
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    private static void fails(Runnable action, RuntimeException sentinel) {
        try { action.run(); throw new AssertionError("missing failure"); }
        catch (RuntimeException error) { check(error == sentinel); failures++; }
    }
    @SuppressWarnings("rawtypes")
    public static void main(String[] args) throws Exception {
        Counter<String> counter = new Counter<>();
        CountView<String> typed = counter;
        CountView raw = counter;
        for (int count : new int[]{0, 7, -4}) {
            counter.count = count;
            check(counter.size() == count); cases++;
            check(typed.size() == count); cases++;
            check(raw.size() == count); cases++;
        }
        check(counter.calls == 9);
        RuntimeException sentinel = new RuntimeException("owned failure");
        counter.failure = sentinel;
        fails(() -> counter.size(), sentinel);
        fails(() -> typed.size(), sentinel);
        fails(() -> raw.size(), sentinel);
        check(counter.calls == 12);
        Child<String> child = new Child<>();
        Counter<String> base = child;
        CountView<String> api = child;
        child.count = 2;
        check(child.size() == 12); cases++;
        check(base.size() == 12); cases++;
        check(api.size() == 12); cases++;
        check(child.calls == 3 && child.overrides == 3);
        child.failure = sentinel;
        fails(() -> api.size(), sentinel);
        check(child.calls == 4 && child.overrides == 4);
        Method bridge = Counter.class.getDeclaredMethod("size");
        check(bridge.isBridge() == args[0].equals("original"));
        check(!bridge.isSynthetic() && Modifier.isFinal(bridge.getModifiers()));
        check(bridge.getReturnType() == int.class && bridge.getGenericReturnType() == int.class);
        check(Counter.class.getDeclaredMethod("getCount").getReturnType() == int.class);
        check(Child.class.getMethod("size").getDeclaringClass() == Counter.class);
        for (Method own : Child.class.getDeclaredMethods())
            check(!own.getName().equals("size") && !own.isBridge());
        ParameterizedType parent = (ParameterizedType) Child.class.getGenericSuperclass();
        ParameterizedType childApi = (ParameterizedType) Child.class.getGenericInterfaces()[0];
        check(parent.getRawType() == Counter.class && childApi.getRawType() == CountView.class);
        check(parent.getActualTypeArguments()[0].equals(Child.class.getTypeParameters()[0]));
        check(childApi.getActualTypeArguments()[0].equals(Child.class.getTypeParameters()[0]));
        ParameterizedType edge = (ParameterizedType) Counter.class.getGenericInterfaces()[0];
        check(edge.getRawType() == CountView.class
              && edge.getActualTypeArguments()[0].equals(Counter.class.getTypeParameters()[0]));
        System.out.println("values=" + cases + ",failures=" + failures);
        System.out.println("plain-integer-bridge=matched,virtual-override=retained,flags=reported");
    }
}
