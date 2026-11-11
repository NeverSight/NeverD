import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public class NonReturningIntegerAliasHarness {
    public static class Concrete extends Policy {
        public Concrete(String message) { super(message); }
    }
    public static class OverridingPolicy extends Policy {
        public int calls;
        public OverridingPolicy(String message) { super(message); }
        @Override public int measure(Scope scope, List items, int size) { calls++; return size; }
    }
    private static int failures;
    private static int values;
    private static final Set<Throwable> fresh = Collections.newSetFromMap(new IdentityHashMap<Throwable, Boolean>());
    private static void fail(Runnable action, String message) {
        try { action.run(); throw new AssertionError("missing terminal failure"); }
        catch (IllegalStateException error) {
            if (message == null || !message.equals(error.getMessage()) || !fresh.add(error))
                throw new AssertionError("terminal message or allocation changed", error);
            failures++;
        } catch (NullPointerException error) {
            if (message != null || !fresh.add(error)) throw new AssertionError("null field failure changed", error);
            failures++;
        }
    }
    private static void invoke(Method method, Object receiver, Object... args) {
        try { method.invoke(receiver, args); throw new AssertionError("missing reflective failure"); }
        catch (InvocationTargetException error) {
            Throwable cause = error.getCause();
            if (cause instanceof RuntimeException) throw (RuntimeException) cause;
            throw new AssertionError(cause);
        } catch (IllegalAccessException error) { throw new AssertionError(error); }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        String alias = args.length == 0 ? "measure" : args[0];
        Method terminal = Policy.class.getDeclaredMethod(alias, Scope.class, List.class, int.class);
        if (args.length == 0) {
            for (Method method : Policy.class.getDeclaredMethods())
                if (method.getName().equals(alias) && method.getReturnType() == Void.class) terminal = method;
        }
        if (terminal.getReturnType() != Void.class || terminal.getGenericReturnType() != Void.class)
            throw new AssertionError("original Void descriptor changed");
        ParameterizedType container = (ParameterizedType) terminal.getGenericParameterTypes()[1];
        WildcardType element = (WildcardType) container.getActualTypeArguments()[0];
        if (container.getRawType() != List.class || !Arrays.equals(element.getUpperBounds(), new Type[]{Item.class}))
            throw new AssertionError("full original container contract changed");
        Method bridge = null;
        for (Method method : Policy.class.getDeclaredMethods())
            if (method.getName().equals("measure") && method.getReturnType() == int.class) bridge = method;
        if (bridge == null || bridge.isBridge() != (args.length == 0)
                || bridge.isSynthetic() != (args.length == 0))
            throw new AssertionError("bridge flags or runtime slot changed");
        final Method typed = terminal, erased = bridge;
        for (String message : new String[]{"", "owned message", null}) {
            Policy policy = new Concrete(message);
            Api api = policy;
            for (Scope scope : new Scope[]{null, new Scope()})
                for (List list : new List[]{null, Collections.emptyList(), Collections.singletonList(new Item()), Collections.singletonList("wrong")})
                    for (int size : new int[]{Integer.MIN_VALUE, -3, 0, 11, Integer.MAX_VALUE}) {
                        fail(() -> policy.measure(scope, list, size), message);
                        fail(() -> api.measure(scope, list, size), message);
                        fail(() -> invoke(erased, policy, scope, list, size), message);
                        fail(() -> invoke(typed, policy, scope, list, size), message);
                    }
        }
        OverridingPolicy override = new OverridingPolicy(null);
        Api api = override;
        for (int size : new int[]{Integer.MIN_VALUE, -3, 0, 11, Integer.MAX_VALUE}) {
            if (override.measure(null, null, size) != size || api.measure(null, null, size) != size)
                throw new AssertionError("original integer virtual override changed");
            values += 2;
        }
        if (override.calls != 10 || failures != 480 || fresh.size() != 480)
            throw new AssertionError("terminal alias coverage changed");
        System.out.println("values=" + values + ",failures=" + failures + ",fresh-exceptions=" + fresh.size());
        System.out.println("void-descriptor-and-full-contract=retained,integer-override=matched,source-name=reported");
    }
}
