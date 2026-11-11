import fixture.ScopeFacade__ScopePartKt;
import fixture.Target;
import fixture.Watcher;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.ParameterizedType;

public final class KotlinPrimitivePartHarness {
    private static final class Recording implements Watcher<Object> {
        Object expected;
        RuntimeException failure;
        int count;
        public void accept(Object element) {
            require(element == expected);
            ++count;
            if (failure != null) throw failure;
        }
    }
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("primitive package part behavior changed");
    }
    public static void main(String[] args) throws Exception {
        var method = ScopeFacade__ScopePartKt.class.getMethod("repeat", Target.class, int.class, kotlin.coroutines.CoroutineContext.class, Object.class);
        require(method.getTypeParameters().length == 0 && method.getReturnType() == Target.class);
        Object[] values = {null, "owned", Integer.valueOf(29), new Object()};
        for (int i = 0; i < values.length; ++i) {
            kotlin.coroutines.CoroutineContext context = new kotlin.coroutines.CoroutineContext() {};
            Target<Object> target = new Target<Object>();
            Recording callback = new Recording();
            target.callback = callback;
            callback.expected = values[i];
            require(method.invoke(null, target, i + 1, context, values[i]) == target && target.watcher != null);
            for (int round = 0; round < 2; ++round) {
                Object next = values[(i + round + 1) % values.length];
                target.fire(next);
                require(target.context == context && target.last == next && callback.count == (round + 1) * (i + 1));
            }
            Object next = new Object();
            callback.failure = new IllegalStateException("owned callback sentinel");
            try { target.fire(next); throw new AssertionError("callback failure missing"); }
            catch (RuntimeException actual) {
                require(actual == callback.failure && target.context == context && target.last == next && callback.count == 2 * (i + 1) + 1);
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
        Object token = new Object();
        for (int count : new int[] {0, -3}) {
            require(method.invoke(null, target, count, null, new Object()) == target);
            target.fire(token);
            require(target.last == token && target.context == null);
        }
        method.invoke(null, target, 1, null, new Object());
        try { target.fire(token); throw new AssertionError("null callback failure missing"); }
        catch (NullPointerException expected) { require(target.last == token && target.context == null); }
        try { method.invoke(null, null, 1, null, new Object()); throw new AssertionError("null target failure missing"); }
        catch (InvocationTargetException expected) { require(expected.getCause() instanceof NullPointerException); }
        System.out.println("zero-and-negative-counts=matched;null-failures=matched;store-before-callback=matched");
    }
}
