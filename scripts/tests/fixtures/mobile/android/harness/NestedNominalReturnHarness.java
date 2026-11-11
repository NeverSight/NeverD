import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public final class NestedNominalReturnHarness {
    private static int values, identities, failures;
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("iterator state changed");
        values++;
    }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("iterator identity changed");
        identities++;
    }
    private static void failure(Runnable call, Class<? extends Throwable> kind,
                                RuntimeException same) {
        try { call.run(); throw new AssertionError("failure missing"); }
        catch (Throwable error) {
            if (error.getClass() != kind || (same != null && error != same))
                throw new AssertionError("failure type or identity changed", error);
            if (same != null) identities++;
            failures++;
        }
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    public static void main(String[] args) {
        View view = new View();
        Iterable<?> inherited = view;
        Key<?> first = new Key<Object>();
        Key<?> second = new Key<String>();
        Object payload = new Object();
        List<Object> original = Arrays.asList(new AbstractMap.SimpleEntry(first, payload),
                                             new AbstractMap.SimpleEntry(second, null), null);
        int calls = 0;
        for (int repetition = 0; repetition < 32; repetition++)
            for (boolean direct : new boolean[] {false, true}) {
                List<Object> backing = new ArrayList<>(original);
                Iterator<?> iterator = backing.iterator();
                view.value = (Iterator) iterator;
                Iterator<?> actual = direct ? view.iterator() : inherited.iterator();
                identity(actual, iterator); value(view.calls, ++calls);
                for (Object expected : original) {
                    Object element = actual.next();
                    identity(element, expected);
                    if (element != null) {
                        Map.Entry entry = (Map.Entry) element;
                        identity(entry.getKey(), ((Map.Entry) expected).getKey());
                        identity(entry.getValue(), ((Map.Entry) expected).getValue());
                    }
                }
                failure(() -> actual.next(), NoSuchElementException.class, null);
                actual.remove(); value(backing.size(), 2);
            }
        view.value = null;
        identity(view.iterator(), null); value(view.calls, ++calls);
        identity(inherited.iterator(), null); value(view.calls, ++calls);
        RuntimeException problem = new IllegalStateException("owned nominal iterator failure");
        view.failure = problem;
        failure(() -> view.iterator(), IllegalStateException.class, problem); value(view.calls, ++calls);
        failure(() -> inherited.iterator(), IllegalStateException.class, problem); value(view.calls, ++calls);
        View absent = null;
        failure(() -> absent.iterator(), NullPointerException.class, null);
        Type result = View.class.getDeclaredMethods()[0].getGenericReturnType();
        for (Method method : View.class.getDeclaredMethods())
            if (method.getName().equals("iterator")) result = method.getGenericReturnType();
        ParameterizedType entry = (ParameterizedType) ((ParameterizedType) result).getActualTypeArguments()[0];
        Type[] arguments = entry.getActualTypeArguments();
        boolean rebuilt = args[0].equals("rebuilt");
        if (rebuilt ? !(arguments[0] instanceof WildcardType) || !(arguments[1] instanceof WildcardType) :
                       !(arguments[0] instanceof ParameterizedType) || arguments[1] != Object.class)
            throw new AssertionError("nested variance differs from report");
        ParameterizedType key = (ParameterizedType) (rebuilt ?
            ((WildcardType) arguments[0]).getUpperBounds()[0] : arguments[0]);
        if (key.getRawType() != Key.class || !(key.getActualTypeArguments()[0] instanceof WildcardType))
            throw new AssertionError("complete nominal key contract changed");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(rebuilt ? "reflection=reported-nested-return" : "reflection=original");
    }
}
