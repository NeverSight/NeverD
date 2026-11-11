import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public final class NestedReturnVarianceHarness {
    private static int identities, values, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("identity changed");
        identities++;
    }
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("value changed");
        values++;
    }
    private static void reflection(boolean recovered) throws Exception {
        TypeVariable<?>[] formals = View.class.getTypeParameters();
        ParameterizedType contract = (ParameterizedType) View.class.getGenericInterfaces()[0];
        ParameterizedType required = (ParameterizedType) contract.getActualTypeArguments()[0];
        ParameterizedType result = (ParameterizedType) View.class.getMethod("values").getGenericReturnType();
        ParameterizedType pair = (ParameterizedType) result.getActualTypeArguments()[0];
        ParameterizedType field = (ParameterizedType) View.class.getField("value").getGenericType();
        ParameterizedType fieldPair = (ParameterizedType) field.getActualTypeArguments()[0];
        for (int i = 0; i < 2; ++i) {
            Type expected = required.getActualTypeArguments()[i];
            if (!(expected instanceof WildcardType)) throw new AssertionError("interface wildcard lost");
            if (!((WildcardType) expected).getUpperBounds()[0].equals(formals[i]))
                throw new AssertionError("interface scope changed");
            Type actual = pair.getActualTypeArguments()[i];
            if (recovered) {
                if (!expected.equals(actual)) throw new AssertionError("return projection differs");
            } else if (!formals[i].equals(actual)) throw new AssertionError("original return changed");
            if (!fieldPair.getActualTypeArguments()[i].equals(formals[i]))
                throw new AssertionError("field metadata changed");
        }
        if (View.class.getMethod("values").isBridge()) throw new AssertionError("invented bridge");
        value(View.class.getMethod("values").getExceptionTypes().length, 0);
    }
    public static void main(String[] args) throws Exception {
        View<String,Integer> view = new View<>();
        Contract<Pair<? extends String,? extends Integer>> typed = view;
        Contract raw = view;
        Pair<String,Integer> first = new Pair<>();
        first.first = "first";
        first.second = 7;
        Pair<String,Integer> second = new Pair<>();
        second.first = "second";
        second.second = 9;
        List<Pair<String,Integer>> rows = new ArrayList<>();
        rows.add(first); rows.add(second);
        view.value = rows.iterator();
        identity(view.values(), view.value);
        identity(typed.values(), view.value);
        identity(raw.values(), view.value);
        Iterator<Pair<? extends String,? extends Integer>> result = typed.values();
        identity(result.next(), first);
        identity(result.next(), second);
        value(first.second, 7);
        value(second.second, 9);
        value(view.calls, 4);
        view.value = null;
        identity(view.values(), null);
        identity(typed.values(), null);
        identity(raw.values(), null);
        value(view.calls, 7);
        view.failure = new IllegalStateException("owned failure");
        for (int i = 0; i < 3; ++i) {
            try {
                if (i == 0) view.values();
                else if (i == 1) typed.values();
                else raw.values();
                throw new AssertionError("missing failure");
            } catch (IllegalStateException e) { identity(e, view.failure); failures++; }
            value(view.calls, 8 + i);
        }
        view.failure = null;
        view.value = rows.iterator();
        result = typed.values();
        identity(result.next(), first);
        result.remove();
        value(rows.size(), 1);
        identity(rows.get(0), second);
        identity(result.next(), second);
        value(view.calls, 11);
        try { result.next(); throw new AssertionError("iterator failure lost"); }
        catch (NoSuchElementException e) { failures++; }
        reflection(args[0].equals("recovered"));
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("nested-return=matched,metadata-difference=reported,no-bridges=retained");
    }
}
