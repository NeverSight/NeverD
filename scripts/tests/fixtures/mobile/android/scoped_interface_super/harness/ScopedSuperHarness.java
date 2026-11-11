import fixture.Base;
import fixture.Child;
import fixture.Trace;
import fixture.View;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.AbstractList;

public final class ScopedSuperHarness {
    public static final class Derived extends Child<String> {
        int queries;
        @Override public String get(int index) { ++queries; throw new AssertionError("unexpected get"); }
        @Override public int size() { ++queries; throw new AssertionError("unexpected size"); }
        @Override public View<String> subList(int from, int to) {
            Trace.hit(1);
            return super.subList(from, to);
        }
    }
    private static final class SpyView extends AbstractList<Object> implements View<Object> {
        int queries;
        public Object get(int index) { ++queries; throw new AssertionError("unexpected result get"); }
        public int size() { ++queries; throw new AssertionError("unexpected result size"); }
    }
    private static void require(boolean value) {
        if (!value) throw new AssertionError("owned scoped interface super changed");
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Class<?> owner = Class.forName("fixture.Child", false, ScopedSuperHarness.class.getClassLoader());
        Method method = owner.getDeclaredMethod("subList", int.class, int.class);
        MethodHandle handle = MethodHandles.publicLookup().findVirtual(owner, "subList",
                MethodType.methodType(View.class, int.class, int.class));
        require(method.getReturnType() == View.class && method.getGenericReturnType().getTypeName().equals("fixture.View<E>"));
        require(owner.getGenericInterfaces()[0].getTypeName().equals("fixture.Facade<E>"));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Trace.events = 0; Trace.wrong = false; Trace.seen = null; Trace.after = null;
            Trace.first = 0; Trace.second = 0;
            int fail = mode >= 3 && mode <= 14 ? 1 + (mode - 3) / 2 : 0;
            Throwable failure = (mode >= 3 && mode <= 14 && mode % 2 == 0)
                    ? new AssertionError("sentinel") : new IllegalStateException("sentinel");
            Trace.fail = fail; Trace.failure = failure;
            SpyView value = new SpyView(); Object replacement = new Object();
            Object selected = mode == 1 ? null : mode == 2 ? new Object() : value;
            Trace.result = selected; Trace.replacement = replacement;
            Derived actual = new Derived(); Child<String> receiver = mode == 16 ? null : actual;
            int from = Integer.MIN_VALUE + mode, to = -7 - mode;
            Object output = null; Throwable caught = null;
            try {
                if (route == 0) output = ScopedSuperCaller.run(receiver, from, to);
                else {
                    if (route == 1) output = method.invoke(receiver, from, to);
                    else if (route == 2) output = handle.invoke(receiver, from, to);
                    else output = ((Base<String>)receiver).subList(from, to);
                    Trace.hit(6);
                }
            } catch (InvocationTargetException error) { caught = error.getCause(); }
              catch (Throwable error) { caught = error; }
            int last = mode == 16 ? 0 : mode == 2 ? 3 : fail == 0 ? 6 : fail;
            int events = attempt == 0 ? 89 : 0;
            for (int step = 1; step <= last; ++step) events = events * 10 + step;
            require(Trace.events == events && !Trace.wrong && value.queries == 0 && actual.queries == 0);
            require(Trace.seen == (last < 3 ? null : actual));
            if (last >= 3) require(Trace.first == from && Trace.second == to);
            require(Trace.result == (last >= 4 ? replacement : selected));
            require(Trace.after == (last >= 5 ? selected : null));
            if (mode == 16) require(caught instanceof NullPointerException);
            else if (mode == 2) require(caught instanceof ClassCastException);
            else require(caught == (fail == 0 ? null : failure));
            require(output == (caught == null || (fail == 6 && route != 0) ? selected : null));
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
