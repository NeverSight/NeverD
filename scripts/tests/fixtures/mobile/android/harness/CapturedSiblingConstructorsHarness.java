import fixture.Owner;
import fixture.Probe;
import fixture.Trace;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;

public final class CapturedSiblingConstructorsHarness {
    private static final class FutureProbe implements Probe<Object> {
        Object value;
        int calls;
        RuntimeException failure;
        public void accept(Object input) {
            value = input;
            ++calls;
            if (failure != null) throw failure;
        }
    }

    private static void check(boolean value) {
        if (!value) throw new AssertionError("captured sibling constructor contract");
    }

    private static Object field(Class<?> owner, String name, Object value) throws Exception {
        Field field = owner.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(value);
    }

    public static void main(String[] args) throws Exception {
        int route = Integer.parseInt(args[0]);
        int mode = Integer.parseInt(args[1]);
        Class<?> base = Class.forName("fixture.Owner$Base");
        Class<?> child = Class.forName("fixture.Owner$Child");
        Constructor<?> ctor = child.getDeclaredConstructor(Owner.class, Probe.class, int.class);
        ctor.setAccessible(true);
        Owner<Object, Object> owner = route == 0 ? new Owner<>() : null;
        Object payload = new Object();
        FutureProbe callback = new FutureProbe();
        RuntimeException expected = new RuntimeException("owned constructor failure");
        for (int attempt = 0; attempt < 2; ++attempt) {
            int stage = mode % 4;
            boolean missing = mode >= 4 && mode < 8;
            int marker = mode >= 8 ? Integer.MIN_VALUE + attempt : 19 + attempt;
            Probe<Object> supplied = missing ? null : callback;
            Trace.events = 0;
            Trace.cached = null;
            Trace.failStage = stage;
            Trace.failure = expected;
            Object result = null;
            try {
                result = ctor.newInstance(owner, supplied, marker);
                check(stage == 0);
            } catch (InvocationTargetException error) {
                check(stage != 0 && error.getCause() == expected);
            }
            Object partial = Trace.cached;
            check(partial != null && partial.getClass() == child);
            check(result == (stage == 0 ? partial : null));
            check(field(child, "this$0", partial) == owner);
            check(field(base, "this$0", partial) == owner);
            check(((Integer) field(base, "marker", partial)).intValue() ==
                  (stage == 1 ? 0 : marker));
            Object retained = field(base, "sink", partial);
            check(retained == (stage == 1 || stage == 2 ? null : supplied));
            int events = stage == 1 ? 1 : stage == 2 ? 9 : 66;
            check(Trace.events == events);
            if (retained != null) {
                int before = callback.calls;
                callback.failure = attempt == 0 ? null : expected;
                try {
                    ((Probe<Object>) retained).accept(payload);
                    check(attempt == 0);
                } catch (RuntimeException error) {
                    check(attempt == 1 && error == expected);
                }
                check(callback.calls == before + 1 && callback.value == payload);
            }
            check(Trace.events == events && Trace.cached == partial);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt +
                               ",events=" + events);
        }
    }
}
