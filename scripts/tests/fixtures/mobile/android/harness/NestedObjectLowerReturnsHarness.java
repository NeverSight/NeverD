import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;

public final class NestedObjectLowerReturnsHarness {
    private static int values, identities, states, failures;
    private static final Method OWN, ROOT;
    private static final MethodHandle HANDLE;
    static {
        try {
            OWN = View.class.getMethod("values"); ROOT = Contract.class.getMethod("values");
            HANDLE = MethodHandles.lookup().findVirtual(View.class, "values", MethodType.methodType(Iterator.class));
        } catch (Exception error) { throw new AssertionError(error); }
    }
    private static void same(Object expected, Object actual) {
        if (expected != actual) throw new AssertionError("identity changed");
        identities++;
    }
    private static void state(int expected, int actual) {
        if (expected != actual) throw new AssertionError("state changed " + expected + ":" + actual);
        states++;
    }
    private static Iterator<?> call(int route, View value) throws Throwable {
        try {
            switch (route) {
                case 0: return value.values();
                case 1: return ((Contract) value).values();
                case 2: return ((Contract<Pair<? super Object,? super Object>>) value).values();
                case 3: return (Iterator<?>) OWN.invoke(value);
                case 4: return (Iterator<?>) ROOT.invoke(value);
                case 5: return (Iterator<?>) HANDLE.invokeExact(value);
                case 6: return value.call();
                case 7: return value.entry();
                case 8: return Caller.call(value);
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void metadata(boolean original) throws Exception {
        ParameterizedType contract = (ParameterizedType) View.class.getGenericInterfaces()[0];
        ParameterizedType required = (ParameterizedType) contract.getActualTypeArguments()[0];
        ParameterizedType result = (ParameterizedType) OWN.getGenericReturnType();
        ParameterizedType pair = (ParameterizedType) result.getActualTypeArguments()[0];
        ParameterizedType field = (ParameterizedType) View.class.getField("value").getGenericType();
        ParameterizedType stored = (ParameterizedType) field.getActualTypeArguments()[0];
        for (int i = 0; i < 2; i++) {
            Type wanted = required.getActualTypeArguments()[i];
            if (!(wanted instanceof WildcardType) || !Arrays.equals(((WildcardType) wanted).getLowerBounds(), new Type[]{Object.class})
                    || !Arrays.equals(((WildcardType) wanted).getUpperBounds(), new Type[]{Object.class})
                    || pair.getActualTypeArguments()[i] != Object.class
                    || stored.getActualTypeArguments()[i] != Object.class)
                throw new AssertionError("complete original bound or original return Signature changed");
        }
        if (OWN.isBridge() || OWN.isSynthetic() || !ROOT.isDefault() || !View.class.getMethod("call").isDefault()
                || View.class.getTypeParameters().length != 0 || !Arrays.equals(OWN.getExceptionTypes(), new Class<?>[0]))
            throw new AssertionError("original runtime roles changed");
    }
    private static void run(int route) throws Throwable {
        int[] marker = {7};
        View view = new View(); view.marker = marker;
        Pair<Object,Object> first = new Pair<>(), second = new Pair<>();
        first.first = marker; first.second = marker;
        second.first = first; second.second = second;
        ArrayList<Pair<Object,Object>> rows = new ArrayList<>(); rows.add(first); rows.add(second);
        Iterator<Pair<Object,Object>> original = rows.iterator(); view.value = original;
        same(original, call(route, view)); same(original, call(route, view));
        state(2, view.calls); state(13, marker[0]); same(marker, view.observed);
        same(first, original.next()); same(marker, first.first); same(first.first, first.second);
        original.remove(); state(1, rows.size()); same(second, rows.get(0));
        first.first = second; same(second, first.first);
        same(second, call(route, view).next()); same(first, second.first); same(second, second.second);
        state(3, view.calls); state(16, marker[0]);
        if (original.hasNext()) throw new AssertionError("iterator state changed"); values++;
        try { original.next(); throw new AssertionError("exhausted iterator accepted"); }
        catch (NoSuchElementException expected) { failures++; }
        view.value = null; same(null, call(route, view)); state(4, view.calls); state(19, marker[0]);
        RuntimeException failure = new RuntimeException("owned-value"); view.failure = failure;
        try { call(route, view); throw new AssertionError("original failure missing"); }
        catch (RuntimeException error) { same(failure, error); failures++; }
        state(5, view.calls); state(22, marker[0]); same(marker, view.observed); same(marker, view.marker);
        view.failure = null; original = rows.iterator(); view.value = original;
        same(original, call(route, view)); same(second, original.next());
        state(6, view.calls); state(25, marker[0]);
        try { call(route, null); throw new AssertionError("null receiver accepted"); }
        catch (NullPointerException expected) { failures++; }
        state(6, view.calls); state(25, marker[0]);
        if (original.hasNext()) throw new AssertionError("returned iterator changed"); values++;
    }
    public static void main(String[] args) throws Throwable {
        metadata(args[0].equals("original"));
        for (int route = 0; route < 9; route++) run(route);
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("complete-object-lower-bound=retained,original-default-and-caller-dispatch=retained,all-original-generic-metadata=unchanged");
    }
}
