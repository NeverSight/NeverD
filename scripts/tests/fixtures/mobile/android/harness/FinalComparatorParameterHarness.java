import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public final class FinalComparatorParameterHarness {
    private static int values, identities, failures;
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("integer result changed");
        values++;
    }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("pair identity changed");
        identities++;
    }
    private static void events(Receiver<?> receiver, int expected) {
        if (receiver.events != expected) throw new AssertionError("comparator effect order changed");
        receiver.events = 0;
    }
    private static void failure(Receiver<?> receiver, Runnable call,
                                Class<? extends Throwable> kind, RuntimeException same, int effects) {
        try { call.run(); throw new AssertionError("failure missing"); }
        catch (Throwable error) {
            if (error.getClass() != kind || (same != null && error != same))
                throw new AssertionError("exception type or identity changed", error);
            if (same != null) identities++;
            failures++;
        }
        events(receiver, effects);
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Receiver<Object> receiver = new Receiver<>();
        Comparator raw = receiver;
        Pair[] pairs = {null, new Pair(new Value(), Arrays.asList(new Node())),
                        new Pair(new Value(), Collections.emptyList())};
        for (int answer : new int[] {Integer.MIN_VALUE, Integer.MAX_VALUE, -1, 0, 1})
            for (Pair first : pairs)
                for (Pair second : pairs) {
                    receiver.answer = answer;
                    value(raw.compare(first, second), answer);
                    identity(receiver.first, first); identity(receiver.second, second); events(receiver, 1);
                    value(receiver.compare(first, second), answer);
                    identity(receiver.first, first); identity(receiver.second, second); events(receiver, 1);
                }
        failure(receiver, () -> raw.compare("bad", pairs[1]), ClassCastException.class, null, 0);
        failure(receiver, () -> raw.compare(pairs[1], "bad"), ClassCastException.class, null, 0);
        RuntimeException problem = new IllegalStateException("owned comparator failure");
        receiver.failure = problem;
        failure(receiver, () -> raw.compare(pairs[1], pairs[2]), IllegalStateException.class, problem, 1);
        failure(receiver, () -> receiver.compare(pairs[1], pairs[2]), IllegalStateException.class, problem, 1);
        receiver.failure = null;
        Receiver<?> absent = null;
        failure(receiver, () -> absent.compare((Object) pairs[1], (Object) pairs[2]), NullPointerException.class, null, 0);
        failure(receiver, () -> absent.compare(pairs[1], pairs[2]), NullPointerException.class, null, 0);
        Method target = Receiver.class.getMethod("compare", Pair.class, Pair.class);
        boolean rebuilt = args[0].equals("rebuilt");
        for (Type parameter : target.getGenericParameterTypes()) {
            Type[] bounds = ((ParameterizedType) parameter).getActualTypeArguments();
            if (rebuilt ? !(bounds[0] instanceof WildcardType) ||
                !Arrays.equals(((WildcardType) bounds[0]).getUpperBounds(), new Type[] {Value.class}) :
                bounds[0] != Value.class) throw new AssertionError("final pair bound differs from report");
            ParameterizedType list = (ParameterizedType) ((WildcardType) bounds[1]).getUpperBounds()[0];
            if (list.getRawType() != List.class || list.getActualTypeArguments()[0] != Node.class)
                throw new AssertionError("unchanged nested list bound changed");
        }
        if (target.getGenericReturnType() != int.class ||
            !Receiver.class.getMethod("compare", Object.class, Object.class).isBridge())
            throw new AssertionError("original primitive return or bridge ABI changed");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(rebuilt ? "reflection=reported-final-parameters,int-return=retained" : "reflection=original");
    }
}
