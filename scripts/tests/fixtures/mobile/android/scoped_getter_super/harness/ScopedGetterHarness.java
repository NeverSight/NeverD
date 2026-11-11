import fixture.Builder;
import fixture.Child;
import fixture.Node;
import fixture.Path;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class ScopedGetterHarness {
    public static final class Derived extends Builder<Object,Object> {
        @Override public Node<Object,Object> fetch() { ++Trace.derived; return super.fetch(); }
    }
    private static void require(boolean value) {
        if (!value) throw new AssertionError("owned scoped getter super changed");
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; ++attempt) {
            Builder<Object,Object> original = new Derived();
            Builder<Object,Object> builder = mode == 16 ? null : original;
            Builder<Object,Object> other = new Builder<Object,Object>();
            Node<Object,Object> node = new Node<Object,Object>(new Object());
            Node<Object,Object> alternate = new Node<Object,Object>(new Object());
            Object selected = mode == 1 || mode == 18 ? null : mode == 19 ? new Object() : node;
            Path<Object,Object,Object> changed = new Path<Object,Object,Object>(new Object());
            Path<Object,Object,Object>[] path = mode == 17 || mode == 18 ? null : mode == 20 ? new Path[0] : new Path[]{new Path(new Object())};
            Path[] replacementPath = new Path[]{new Path(new Object())};
            int fail = mode >= 2 && mode <= 15 ? 1 + (mode - 2) / 2 : 0;
            Throwable failure = mode >= 2 && mode <= 15 && mode % 2 == 1 ? new AssertionError("sentinel") : new IllegalStateException("sentinel");
            Trace.events = 0; Trace.fail = fail; Trace.failure = failure; Trace.derived = 0;
            Trace.returned = selected; Trace.alternate = alternate;
            Trace.getterReceiver = null; Trace.stampReceiver = null; Trace.parent = null; Trace.element = null;
            Trace.builder = builder; Trace.other = other; Trace.path = path;
            Trace.replacementPath = replacementPath; Trace.changed = changed; Trace.stampValue = 43;
            Object output = null; Throwable caught = null;
            try {
                if (route == 0) output = ScopedGetterCaller.make(builder, path);
                else if (route == 1) output = Child.class.getConstructor(Builder.class, Path[].class).newInstance(builder, path);
                else if (route == 2) output = MethodHandles.publicLookup().findConstructor(Child.class,
                        MethodType.methodType(void.class, Builder.class, Path[].class)).invoke(builder, path);
                else output = new Child<Object,Object,Object>(builder, path);
            } catch (InvocationTargetException error) { caught = error.getCause(); }
              catch (Throwable error) { caught = error; }
            int events = attempt == 0 ? 89 : 0, last = 0;
            if (builder != null) {
                int[] steps = selected == null ? new int[]{1,2,3,5,6,7} : new int[]{1,2,3,4,5,6,7};
                for (int step : steps) {
                    events = events * 10 + step; last = step;
                    if (step == fail || step == 1 && mode == 19 || step == 3 && (mode == 17 || mode == 20)) break;
                }
            }
            require(Trace.events == events && Trace.derived == (builder == null ? 0 : 1));
            if (mode == 16 || mode == 17) require(caught instanceof NullPointerException);
            else if (mode == 19) require(caught instanceof ClassCastException);
            else if (mode == 20) require(caught instanceof ArrayIndexOutOfBoundsException);
            else require(caught == (fail == 0 ? null : failure));
            require(Trace.getterReceiver == (builder == null ? null : original));
            require(Trace.returned == (last >= 2 ? alternate : selected));
            if (last >= 2 || mode == 19) require(Trace.builder == other && Trace.path == replacementPath);
            if (path != null && path.length != 0 && (last >= 2 || mode == 19)) require(path[0] == changed);
            require((Trace.parent != null) == (last >= 3));
            if (Trace.parent != null) {
                require(Trace.parent.node == selected && Trace.parent.path == path);
                Child<?,?,?> partial = (Child<?,?,?>)Trace.parent;
                require(partial.builder == (last >= 6 ? original : null));
                require(partial.stamp == (last == 7 ? 43 : 0));
            }
            require(Trace.stampReceiver == (last >= 6 ? original : null));
            if (last >= 4 && selected != null) require(Trace.element == changed);
            require(output == (caught == null ? Trace.parent : null));
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
