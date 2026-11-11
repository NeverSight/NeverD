import fixture.*;
import java.lang.reflect.*;

public class NonReturningBooleanResultsHarness {
    private static int values, identities, failures;
    private static void value(boolean ok) {
        values++;
        if (!ok) throw new AssertionError("value " + values);
    }
    private static void identity(Object actual, Object expected) {
        identities++;
        if (actual != expected) throw new AssertionError("identity " + identities);
    }
    private static void run(Runnable action, Class<?> type, RuntimeException sentinel, int calls) {
        Effects.calls = 0;
        Effects.failure = sentinel;
        try {
            action.run();
            throw new AssertionError("normal result from terminal target");
        } catch (RuntimeException actual) {
            failures++;
            identity(actual.getClass(), type);
            if (sentinel != null && type == sentinel.getClass()) identity(actual, sentinel);
            value(Effects.calls == calls);
        }
    }
    public static void main(String[] args) throws Exception {
        View<Object> view = new View<Object>();
        Entry<Object> entry = new Entry<Object>();
        Object payload = new Object();
        entry.value = payload;
        RuntimeException sentinel = new IllegalStateException("owned prefix sentinel");
        for (RuntimeException cause : new RuntimeException[] {null, sentinel}) {
            Class<?> thrown = cause == null ? Failure.class : cause.getClass();
            run(() -> Caller.wide(view, entry), thrown, cause, 1);
            run(() -> Caller.narrow(view, entry), thrown, cause, 1);
            run(() -> Caller.wide(view, null), thrown, cause, 1);
            run(() -> Caller.narrow(view, null), thrown, cause, 1);
            run(() -> Caller.wide(view, new Object()), ClassCastException.class, cause, 0);
            run(() -> Caller.wide(null, entry), NullPointerException.class, cause, 0);
            run(() -> Caller.narrow(null, entry), NullPointerException.class, cause, 0);
            java.util.Collection<Entry<Object>> entries = java.util.Collections.singletonList(entry);
            run(() -> Caller.wideAll(view, entries), thrown, cause, 1);
            run(() -> Caller.narrowAll(view, entries), thrown, cause, 1);
            run(() -> Caller.wideAll(view, null), thrown, cause, 1);
            run(() -> Caller.narrowAll(view, null), thrown, cause, 1);
            run(() -> Caller.wideAll(null, entries), NullPointerException.class, cause, 0);
            run(() -> Caller.narrowAll(null, entries), NullPointerException.class, cause, 0);
            run(() -> Caller.wideScalar(view, payload), thrown, cause, 1);
            run(() -> Caller.narrowScalar(view, payload), thrown, cause, 1);
            run(() -> Caller.wideScalar(view, null), thrown, cause, 1);
            run(() -> Caller.narrowScalar(view, null), thrown, cause, 1);
            run(() -> Caller.wideScalar(null, payload), NullPointerException.class, cause, 0);
            run(() -> Caller.narrowScalar(null, payload), NullPointerException.class, cause, 0);
            identity(entry.value, payload);
        }
        String prefix = args.length == 0 ? "" : "__neverdNonReturning_";
        Method typed = View.class.getDeclaredMethod(prefix + "add", Entry.class);
        Class<?> expectedReturn = Void.class;
        identity(typed.getReturnType(), expectedReturn);
        identity(typed.getGenericReturnType(), expectedReturn);
        value(!typed.isBridge());
        Method bridge = View.class.getDeclaredMethod("add", Object.class);
        value(bridge.isBridge() && bridge.isSynthetic());
        identity(bridge.getReturnType(), boolean.class);
        ParameterizedType parameter = (ParameterizedType) typed.getGenericParameterTypes()[0];
        identity(parameter.getRawType(), Entry.class);
        identity(parameter.getActualTypeArguments()[0], View.class.getTypeParameters()[0]);
        ParameterizedType parent = (ParameterizedType) View.class.getGenericSuperclass();
        identity(parent.getRawType(), Root.class);
        ParameterizedType element = (ParameterizedType) parent.getActualTypeArguments()[0];
        identity(element.getRawType(), Entry.class);
        identity(element.getActualTypeArguments()[0], View.class.getTypeParameters()[0]);
        Method typedAll = null, bridgeAll = null;
        for (Method candidate : View.class.getDeclaredMethods()) {
            if (!candidate.getName().equals("addAll") && !candidate.getName().equals(prefix + "addAll")) continue;
            if (candidate.getReturnType() == expectedReturn) typedAll = candidate;
            if (candidate.getReturnType() == boolean.class) bridgeAll = candidate;
        }
        if (typedAll == null || bridgeAll == null || bridgeAll.equals(typedAll)) throw new AssertionError("missing same-parameter entry");
        identity(typedAll.getReturnType(), expectedReturn);
        value(!typedAll.isBridge());
        value(bridgeAll.isBridge() == (args.length == 0 || args[0].equals("verified")));
        ParameterizedType collection = (ParameterizedType) typedAll.getGenericParameterTypes()[0];
        identity(collection.getRawType(), java.util.Collection.class);
        ParameterizedType item = (ParameterizedType) ((WildcardType) collection.getActualTypeArguments()[0]).getUpperBounds()[0];
        identity(item.getRawType(), Entry.class);
        identity(item.getActualTypeArguments()[0], View.class.getTypeParameters()[0]);
        Method typedScalar = null, bridgeScalar = null;
        for (Method candidate : View.class.getDeclaredMethods()) {
            if (!candidate.getName().equals("push") && !candidate.getName().equals(prefix + "push")) continue;
            if (candidate.getReturnType() == expectedReturn) typedScalar = candidate;
            if (candidate.getReturnType() == boolean.class) bridgeScalar = candidate;
        }
        if (typedScalar == null || bridgeScalar == null || bridgeScalar.equals(typedScalar)) throw new AssertionError("missing class-formal entry");
        identity(typedScalar.getReturnType(), expectedReturn);
        value(!typedScalar.isBridge());
        value(bridgeScalar.isBridge() == (args.length == 0 || args[0].equals("verified")));
        identity(typedScalar.getGenericParameterTypes()[0], View.class.getTypeParameters()[0]);
        identity(parent.getActualTypeArguments()[1], View.class.getTypeParameters()[0]);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("nonreturning-prefix-casts-and-exceptions=matched,distinct-void-and-boolean-entries=matched");
    }
}
