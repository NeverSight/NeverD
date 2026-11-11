import fixture.*;
import java.lang.reflect.*;

public final class SubstitutedInterfaceReturnsHarness {
    private static int values, identities, failures;
    private static void value(boolean ok) {
        values++;
        if (!ok) throw new AssertionError("value " + values);
    }
    private static void identity(Object actual, Object expected) {
        identities++;
        if (actual != expected) throw new AssertionError("identity " + identities);
    }
    private static void failure(Runnable action, RuntimeException sentinel, Impl impl, int calls) {
        try {
            action.run();
            throw new AssertionError("missing iterator failure");
        } catch (RuntimeException actual) {
            failures++;
            identity(actual.getClass(), sentinel == null ? NullPointerException.class : sentinel.getClass());
            if (sentinel != null) identity(actual, sentinel);
            value(impl.calls == calls);
        }
    }
    public static void main(String[] args) throws Exception {
        Impl impl = new Impl();
        for (Item current : new Item[] {new Item(), null}) {
            impl.current = current;
            impl.calls = 0;
            identity(Caller.narrow(impl), current);
            value(impl.calls == 1);
            identity(Caller.wide(impl), current);
            value(impl.calls == 2);
            identity(impl.current, current);
        }
        RuntimeException sentinel = new IllegalStateException("owned iterator failure");
        impl.failure = sentinel;
        impl.calls = 0;
        failure(() -> Caller.narrow(impl), sentinel, impl, 1);
        failure(() -> Caller.wide(impl), sentinel, impl, 2);
        impl.failure = null;
        impl.calls = 0;
        failure(() -> Caller.narrow(null), null, impl, 0);
        failure(() -> Caller.wide(null), null, impl, 0);
        Method typed = null, erased = null, concreteBridge = null;
        for (Method method : Narrow.class.getDeclaredMethods()) {
            if (method.getReturnType() == Item.class) typed = method;
            if (method.getReturnType() == Object.class) erased = method;
        }
        if (typed == null) throw new AssertionError("missing typed declaration");
        identity(typed.getReturnType(), Item.class);
        identity(typed.getGenericReturnType(), Item.class);
        value(args.length == 0 ? erased == null : erased != null && erased.isBridge() && erased.isSynthetic() && erased.isDefault());
        ParameterizedType parent = (ParameterizedType) Narrow.class.getGenericInterfaces()[0];
        identity(parent.getRawType(), java.util.Iterator.class);
        identity(parent.getActualTypeArguments()[0], Item.class);
        for (Method method : Impl.class.getDeclaredMethods())
            if (method.getName().equals("next") && method.getReturnType() == Object.class) concreteBridge = method;
        value(concreteBridge != null && concreteBridge.isBridge() && concreteBridge.isSynthetic());
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("typed-and-erased-iterator-dispatch=matched,added-interface-bridge=reported");
    }
}
