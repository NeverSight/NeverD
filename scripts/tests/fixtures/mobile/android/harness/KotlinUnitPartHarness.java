import fixture.ScopeFacade__ScopePartKt;
import fixture.Target;
import fixture.Watcher;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.ParameterizedType;

public final class KotlinUnitPartHarness {
    private static final class Recording implements Watcher<Object> {
        Object expected;
        RuntimeException failure;
        int count;
        public void accept(Object element) {
            if (element != expected) throw new AssertionError("captured element identity changed");
            ++count;
            if (failure != null) throw failure;
        }
    }
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("package part behavior changed");
    }
    public static void main(String[] args) throws Exception {
        var method = ScopeFacade__ScopePartKt.class.getMethod("register", Target.class, Object.class);
        require(method.getTypeParameters().length == 0 && method.getReturnType() == void.class);
        Object[] values = {null, "owned", Integer.valueOf(29), new Object()};
        for (int i = 0; i < values.length; ++i) {
            Target<Object> target = new Target<Object>();
            Recording callback = new Recording();
            target.callback = callback;
            callback.expected = values[i];
            require(method.invoke(null, target, values[i]) == null && target.watcher != null);
            for (int round = 0; round < 2; ++round) {
                Object next = values[(i + round + 1) % values.length];
                target.fire(next);
                require(target.last == next && callback.count == round + 1);
            }
            Object next = new Object();
            callback.failure = new IllegalStateException("owned callback sentinel");
            try { target.fire(next); throw new AssertionError("callback failure missing"); }
            catch (RuntimeException actual) {
                require(actual == callback.failure && target.last == next && callback.count == 3);
            }
            if (i == 0) {
                try {
                    var contract = (ParameterizedType) target.watcher.getClass().getGenericInterfaces()[0];
                    var type = contract.getActualTypeArguments()[0];
                    require(args[0].equals("original") ? type == null : type == Object.class);
                } catch (TypeNotPresentException missing) {
                    require(args[0].equals("original") && missing.typeName().equals("E"));
                }
            }
            System.out.println(i + ":count=" + callback.count);
        }
        Target<Object> target = new Target<Object>();
        method.invoke(null, target, new Object());
        Object token = new Object();
        try { target.fire(token); throw new AssertionError("null callback failure missing"); }
        catch (NullPointerException expected) { require(target.last == token); }
        try { method.invoke(null, null, new Object()); throw new AssertionError("null target failure missing"); }
        catch (InvocationTargetException expected) { require(expected.getCause() instanceof NullPointerException); }
        System.out.println("null-callback-and-target=matched;store-before-callback=matched");
    }
}
