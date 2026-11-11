import fixture.ScopeFacade__ScopePartKt;
import fixture.Target;
import fixture.Watcher;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.ParameterizedType;
import kotlin.coroutines.CoroutineContext;
import kotlin.jvm.functions.Function2;

public final class KotlinSuspendPartHarness {
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
    private static final class Action implements Function2 {
        Object expected;
        Object result;
        RuntimeException failure;
        int count;
        public Object invoke(Object value, Object continuation) {
            require(value == expected && continuation == null);
            ++count;
            if (failure != null) throw failure;
            return result;
        }
    }
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("suspend callback behavior changed");
    }
    public static void main(String[] args) throws Exception {
        var method = ScopeFacade__ScopePartKt.class.getMethod("repeat", Target.class, int.class,
                CoroutineContext.class, Object.class, Function2.class);
        require(method.getTypeParameters().length == 0 && method.getReturnType() == Target.class);
        Object[] values = {null, "owned", Integer.valueOf(29), new Object()};
        for (int i = 0; i < values.length; ++i) {
            CoroutineContext context = new CoroutineContext() {};
            Target<Object> target = new Target<Object>();
            Recording callback = new Recording();
            Action action = new Action();
            target.callback = callback;
            callback.expected = values[i];
            require(method.invoke(null, target, i + 1, context, values[i], action) == target);
            for (int round = 0; round < 2; ++round) {
                action.expected = values[(i + round + 1) % values.length];
                action.result = new Object();
                target.fire(action.expected);
                require(target.context == context && target.last == action.result
                        && callback.count == (round + 1) * (i + 1) && action.count == round + 1);
            }
            callback.failure = new IllegalStateException("owned callback sentinel");
            action.expected = new Object();
            action.result = new Object();
            try { target.fire(action.expected); throw new AssertionError("callback failure missing"); }
            catch (RuntimeException actual) {
                require(actual == callback.failure && target.context == context && target.last == action.result
                        && callback.count == 2 * (i + 1) + 1 && action.count == 3);
            }
            Object previous = target.last;
            Object previousContext = target.context = new Object();
            action.failure = new IllegalArgumentException("owned action sentinel");
            try { target.fire(action.expected); throw new AssertionError("action failure missing"); }
            catch (RuntimeException actual) {
                require(actual == action.failure && target.last == previous && target.context == previousContext
                        && callback.count == 2 * (i + 1) + 1 && action.count == 4);
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
            System.out.println(i + ":callback-count=" + callback.count + ";action-count=" + action.count);
        }
        Target<Object> target = new Target<Object>();
        Action action = new Action();
        Object token = action.expected = action.result = new Object();
        for (int count : new int[] {0, -3}) {
            require(method.invoke(null, target, count, null, new Object(), action) == target);
            target.fire(token);
            require(target.last == token && target.context == null);
        }
        method.invoke(null, target, 1, null, new Object(), action);
        try { target.fire(token); throw new AssertionError("null callback failure missing"); }
        catch (NullPointerException expected) { require(target.last == token && action.count == 3); }
        Object previousContext = target.context = new Object();
        method.invoke(null, target, 1, null, new Object(), null);
        try { target.fire(new Object()); throw new AssertionError("null action failure missing"); }
        catch (NullPointerException expected) {
            require(target.last == token && target.context == previousContext && action.count == 3);
        }
        try { method.invoke(null, null, 1, null, new Object(), action); throw new AssertionError("null target failure missing"); }
        catch (InvocationTargetException expected) { require(expected.getCause() instanceof NullPointerException); }
        System.out.println("counts=matched;action-before-stores=matched;continuation-null=matched;null-failures=matched");
    }
}
