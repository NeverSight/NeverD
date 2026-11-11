import fixture.Trace;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.function.BinaryOperator;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class DependentOperatorHarness {
    private static final RuntimeException RUNTIME = new RuntimeException("owned callback failure");
    private static final Error ERROR = new AssertionError("owned callback error");
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned dependent operator mismatch");
    }
    private static BinaryOperator result(final int which, final int mode, final int[] counts,
            final Map left, final Map right, final Object[] received) {
        return new BinaryOperator<Map>() {
            public Map apply(Map first, Map second) {
                require(Trace.events == 123 && first == left && second == right);
                counts[which + 2]++;
                received[0] = first;
                received[1] = second;
                if (mode == 20) throw RUNTIME;
                if (mode == 21) throw ERROR;
                return mode == 22 ? null : which == 0 ? left : right;
            }
        };
    }
    private static void run(int route, final int mode, int attempt) {
        final Object first = mode == 6 || mode == 8 ? null : mode == 4 ? Integer.valueOf(37) : new String("owned left");
        final Object second = mode == 7 || mode == 8 ? null : mode == 5 ? Integer.valueOf(41) : new String("owned right");
        final Object replacement = new Object();
        final String merged = new String("owned merged");
        final Map left = new LinkedHashMap(), right = new LinkedHashMap();
        left.put(first, merged);
        right.put(second, replacement);
        final int[] counts = new int[4];
        final Object[] received = new Object[2], applied = new Object[2];
        final BinaryOperator original = result(0, mode, counts, left, right, applied);
        final BinaryOperator other = result(1, mode, counts, left, right, applied);
        Trace.targets = 0;
        Trace.events = 0;
        Trace.left = first;
        Trace.right = second;
        Trace.first = Trace.second = Trace.value = null;
        Trace.operator = null;
        Trace.result = mode == 13 ? replacement : mode == 14 ? null : mode == 19 ? other : original;
        BinaryOperator operator = new BinaryOperator<String>() {
            public String apply(String a, String b) {
                require(Trace.events == 1 && a == first && b == second);
                counts[0]++;
                received[0] = a;
                received[1] = b;
                if (mode == 2) throw RUNTIME;
                if (mode == 3) throw ERROR;
                if (mode == 16) Trace.result = other;
                if (mode == 17) { Trace.left = replacement; Trace.right = replacement; }
                if (mode == 18) Trace.action = new Runnable() {
                    public void run() {
                        require(Trace.events == 12 && counts[0] == 1);
                        counts[1]++;
                        Trace.result = other;
                    }
                };
                return mode == 9 ? null : merged;
            }
        };
        Trace.action = mode == 10 ? null : new Runnable() {
            public void run() {
                require(Trace.events == 12 && counts[0] == 1);
                counts[1]++;
                if (mode == 11) throw RUNTIME;
                if (mode == 12) throw ERROR;
                if (mode == 15) Trace.result = other;
            }
        };
        if (mode == 1) operator = null;
        Object value = null;
        Throwable failure = null;
        try { value = DependentOperatorCaller.call(route, operator); }
        catch (Throwable caught) { failure = caught; }
        int stage = mode >= 1 && mode <= 5 ? 1 : mode >= 10 && mode <= 12 ? 2 : 3;
        require(Trace.targets == 1 && Trace.events == (stage == 1 ? 1 : stage == 2 ? 12 : 123));
        require(Trace.operator == operator && Trace.first == first && Trace.second == second);
        require(counts[0] == (mode == 1 || mode == 4 || mode == 5 ? 0 : 1));
        require(counts[1] == (stage >= 2 && mode != 10 ? 1 : 0));
        if (counts[0] != 0) require(received[0] == first && received[1] == second);
        require(Trace.value == (stage >= 2 && mode != 9 ? merged : null));
        boolean alternate = mode == 15 || mode == 16 || mode == 18 || mode == 19;
        Object expected = mode == 13 ? replacement : mode == 14 ? null : alternate ? other : original;
        require(Trace.result == expected);
        require(Trace.left == (mode == 17 ? replacement : first));
        require(Trace.right == (mode == 17 ? replacement : second));
        if (mode == 2 || mode == 11) require(failure == RUNTIME);
        else if (mode == 3 || mode == 12) require(failure == ERROR);
        else if (mode == 1 || mode == 10) require(failure instanceof NullPointerException);
        else if (mode == 4 || mode == 5 || mode == 13) require(failure instanceof ClassCastException);
        else {
            require(failure == null && value == expected);
            if (value != null) {
                Object returned = null;
                Throwable later = null;
                try { returned = ((BinaryOperator)value).apply(mode == 23 ? replacement : left, right); }
                catch (Throwable caught) { later = caught; }
                if (mode == 20) require(later == RUNTIME);
                else if (mode == 21) require(later == ERROR);
                else if (mode == 23) require(later instanceof ClassCastException);
                else require(later == null && returned == (mode == 22 ? null : alternate ? right : left));
                if (mode != 23) require(applied[0] == left && applied[1] == right);
            }
        }
        boolean invoked = failure == null && value != null && mode != 23;
        require(counts[2] == (invoked && !alternate ? 1 : 0));
        require(counts[3] == (invoked && alternate ? 1 : 0));
        require(left.size() == 1 && left.get(first) == merged);
        require(right.size() == 1 && right.get(second) == replacement);
        System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        run(route, mode, 0);
        run(route, mode, 1);
    }
}
