import fixture.Core;
import java.lang.reflect.*;
import java.util.*;

public final class NearerGenericClassCovarianceHarness {
    private static int values, identities, states, failures;
    private interface Call { Core run() throws Throwable; }
    private static final class Leaf extends Core {
        public Leaf clone() { calls += 2; return (Leaf) super.clone(); }
    }
    private static Core invoke(Method method, Object receiver) throws Throwable {
        try { return (Core) method.invoke(receiver); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original clone identity changed");
        identities++;
    }
    private static void run(Call call, Core receiver, int delta, RuntimeException problem, boolean absent) {
        receiver.calls = 0; receiver.seen = null; receiver.failure = problem;
        Object payload = receiver.get("first");
        try {
            Core result = call.run();
            if (problem != null || absent) throw new AssertionError("original clone failure missing");
            if (result == receiver || result.getClass() != receiver.getClass() || result.calls != delta
                    || !new ArrayList<String>(result.keySet()).equals(Arrays.asList("first", "second")))
                throw new AssertionError("SDK shallow clone class, fields or insertion order changed");
            same(result.seen, receiver);
            same(result.get("first"), payload);
            same(result.get("second"), payload);
            result.put("clone-only", receiver);
            if (receiver.containsKey("clone-only")) throw new AssertionError("SDK clone shared its map storage");
            ((List<Object>) payload).add(result);
            same(((List<?>) receiver.get("first")).get(((List<?>) payload).size() - 1), result);
            values++;
        } catch (Throwable error) {
            if (absent) {
                if (error.getClass() != NullPointerException.class)
                    throw new AssertionError("null clone receiver failure changed", error);
            } else same(error, problem);
            if (!absent && problem == null) throw new AssertionError("unexpected clone failure", error);
            failures++;
        }
        if (receiver.calls != (absent ? 0 : delta)) throw new AssertionError("clone virtual dispatch changed");
        same(receiver.seen, absent ? null : receiver);
        same(receiver.get("first"), payload);
        same(receiver.get("second"), payload);
        states++;
    }
    public static void main(String[] args) throws Exception {
        Method typed = null, bridge = null;
        for (Method method : Core.class.getDeclaredMethods())
            if (method.getName().equals("clone")) {
                if (method.isBridge()) bridge = method; else typed = method;
            }
        final Method exact = typed, erased = bridge;
        Method parent = HashMap.class.getDeclaredMethod("clone");
        if (typed == null || bridge == null || !bridge.isSynthetic() || typed.getReturnType() != Core.class
                || bridge.getReturnType() != Object.class || bridge.getTypeParameters().length != 0
                || typed.getExceptionTypes().length != 0 || bridge.getExceptionTypes().length != 0
                || !Core.class.getGenericSuperclass().getTypeName()
                    .equals("java.util.LinkedHashMap<java.lang.String, java.lang.Object>"))
            throw new AssertionError("original nearest class contract or covariant bridge metadata changed");
        for (Core receiver : new Core[] {new Core(), new Leaf()}) {
            List<Object> payload = new ArrayList<Object>();
            receiver.put("first", payload); receiver.put("second", payload);
            int delta = receiver instanceof Leaf ? 3 : 1;
            HashMap<String, Object> view = receiver;
            Call[] paths = {receiver::clone, () -> (Core) view.clone(),
                            () -> invoke(exact, receiver), () -> invoke(erased, receiver),
                            () -> invoke(parent, receiver)};
            for (Call path : paths) {
                run(path, receiver, delta, null, false);
                run(path, receiver, delta, new IllegalStateException("owned clone failure"), false);
            }
            Call[] missing = {() -> ((Core) null).clone(), () -> (Core) ((HashMap<?, ?>) null).clone(),
                              () -> invoke(exact, null), () -> invoke(erased, null), () -> invoke(parent, null)};
            for (Call path : missing) run(path, receiver, delta, null, true);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("nearest-sdk-contract=retained,original-covariant-bridge=retained,shallow-clone-aliases=matched");
    }
}
