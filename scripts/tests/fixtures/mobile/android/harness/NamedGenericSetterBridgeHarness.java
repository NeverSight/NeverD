import fixture.*;
import java.lang.reflect.*;

public final class NamedGenericSetterBridgeHarness {
    private static int identities, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("setter identity changed");
        identities++;
    }
    private static void events(int expected) {
        if (Trace.events != expected) throw new AssertionError("setter effects changed");
        Trace.events = 0;
    }
    private static void failure(Runnable call, RuntimeException same, int effects) {
        try { call.run(); throw new AssertionError("failure missing"); }
        catch (RuntimeException e) {
            if (same != null) identity(e, same);
            else if (e.getClass() != NullPointerException.class) throw new AssertionError(e);
            failures++;
        }
        events(effects);
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Child<Object, Object> receiver = new Child<>(); Base base = receiver;
        for (Object value : new Object[]{new Object(), "text", new int[]{1,2}, null}) {
            receiver.set(value); identity(receiver.value, value); events(1);
            receiver.set$contract(value); identity(receiver.value, value); events(1);
            base.through(value); identity(receiver.value, value); events(31);
        }
        RuntimeException problem = new IllegalStateException("owned setter failure");
        Trace.targetFailure = problem;
        failure(() -> receiver.set(new Object()), problem, 1);
        failure(() -> receiver.set$contract(new Object()), problem, 1);
        failure(() -> base.through(new Object()), problem, 31);
        identity(receiver.value, null);
        Trace.targetFailure = null;
        Child absent = null; Base absentBase = null;
        failure(() -> absent.set(null), null, 0);
        failure(() -> absent.set$contract(null), null, 0);
        failure(() -> absentBase.through(null), null, 0);
        Type s = Child.class.getTypeParameters()[0];
        if (!Child.class.getMethod("set", Object.class).getGenericParameterTypes()[0].equals(s)
                || !((ParameterizedType) Child.class.getGenericSuperclass()).getActualTypeArguments()[0].equals(s)
                || !Base.class.getMethod("set$contract", Object.class).getGenericParameterTypes()[0]
                    .equals(Base.class.getTypeParameters()[0]))
            throw new AssertionError("complete generic contract changed");
        boolean original = args.length != 0 && args[0].equals("original");
        Method bridge = Child.class.getMethod("set$contract", Object.class);
        if (bridge.isBridge() != original || bridge.isSynthetic() != original
                || bridge.getGenericParameterTypes()[0] != Object.class)
            throw new AssertionError("bridge metadata differs from report");
        System.out.println("identities=" + identities + ",failures=" + failures);
        System.out.println(original ? "reflection=original" : "reflection=reported-bridge-flags");
    }
}
