import fixture.*;
import java.lang.reflect.*;

public class NonReturningVoidResultsHarness {
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
        Host<Object> host = new Host<Object>();
        Object payload = new Object();
        host.value = payload;
        Api<Object> api = host.open();
        Api<Object> absent = null;
        RuntimeException sentinel = new IllegalStateException("owned void prefix sentinel");
        for (RuntimeException cause : new RuntimeException[] {null, sentinel}) {
            Class<?> thrown = cause == null ? Failure.class : cause.getClass();
            identity(api.read(), payload);
            run(() -> api.add(payload), thrown, cause, 1);
            run(() -> api.set(payload), thrown, cause, 1);
            run(() -> api.remove(), thrown, cause, 1);
            run(() -> api.narrowAdd(payload), thrown, cause, 1);
            run(() -> api.narrowSet(payload), thrown, cause, 1);
            run(() -> api.narrowRemove(), thrown, cause, 1);
            run(() -> api.add(null), thrown, cause, 1);
            run(() -> api.set(null), thrown, cause, 1);
            run(() -> api.narrowAdd(null), thrown, cause, 1);
            run(() -> api.narrowSet(null), thrown, cause, 1);
            run(() -> absent.add(payload), NullPointerException.class, cause, 0);
            run(() -> absent.set(payload), NullPointerException.class, cause, 0);
            run(() -> absent.remove(), NullPointerException.class, cause, 0);
            run(() -> absent.narrowAdd(payload), NullPointerException.class, cause, 0);
            run(() -> absent.narrowSet(payload), NullPointerException.class, cause, 0);
            run(() -> absent.narrowRemove(), NullPointerException.class, cause, 0);
            identity(api.read(), payload);
            identity(host.value, payload);
        }
        String[] slots = {"add", "set", "remove"};
        for (int i = 0; i < slots.length; i++) {
            Method typed = null, bridge = null;
            String alias = args.length == 0 ? slots[i] : args[i];
            for (Method candidate : api.getClass().getDeclaredMethods()) {
                if (candidate.getName().equals(alias) && candidate.getReturnType() == Void.class) typed = candidate;
                if (candidate.getName().equals(slots[i]) && candidate.getReturnType() == void.class) bridge = candidate;
            }
            if (typed == null || bridge == null) throw new AssertionError("missing original void slot");
            value(!typed.isBridge());
            value(bridge.isBridge() == (args.length == 0));
            identity(typed.getReturnType(), Void.class);
            identity(typed.getGenericReturnType(), Void.class);
            identity(bridge.getReturnType(), void.class);
            identity(bridge.getGenericReturnType(), void.class);
            if (i < 2) {
                identity(typed.getGenericParameterTypes()[0], Host.class.getTypeParameters()[0]);
                identity(bridge.getGenericParameterTypes()[0], args.length == 0 ? Object.class : Host.class.getTypeParameters()[0]);
            }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("nonreturning-void-prefix-dispatch-and-exceptions=matched,source-names-and-parameters=reported");
    }
}
