import fixture.Trace;
import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.stream.Collector;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class DependentCollectorHarness {
    private static final RuntimeException RUNTIME = new RuntimeException("owned callback failure");
    private static final Error ERROR = new AssertionError("owned callback error");
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned dependent collector mismatch");
    }
    private static Collector collector() {
        return Collector.of(ArrayList::new, ArrayList::add,
                (ArrayList a, ArrayList b) -> { a.addAll(b); return a; });
    }
    private static void run(int route, final int mode, int attempt) {
        final Object source = mode == 21 ? null : mode == 22 ? Integer.valueOf(37) : new String("owned source");
        final Object replacement = new Object(), left = new Object(), right = new Object();
        final Collector original = collector(), other = collector();
        final Collection items = new ArrayList();
        final Map index = new LinkedHashMap();
        items.add(left);
        index.put(left, items);
        final int[] counts = new int[5];
        final Object[] received = new Object[2];
        Trace.targets = 0;
        Trace.events = 0;
        Trace.source = source;
        Trace.seen = Trace.left = Trace.right = null;
        Trace.first = Trace.second = Trace.collectionSupplier = Trace.mapSupplier = null;
        Trace.collection = null;
        Trace.map = null;
        Trace.result = mode == 15 ? replacement : mode == 16 ? null : original;
        Function first = new Function<String, Object>() {
            public Object apply(String input) {
                require(Trace.events == 1 || mode == 23 && Trace.events == 12);
                counts[0]++;
                received[0] = input;
                if (mode == 5) throw RUNTIME;
                if (mode == 6) throw ERROR;
                if (mode == 19) Trace.source = replacement;
                return left;
            }
        };
        Function second = new Function<String, Object>() {
            public Object apply(String input) {
                require(Trace.events == 12 && counts[0] == 1);
                counts[1]++;
                received[1] = input;
                if (mode == 7) throw RUNTIME;
                if (mode == 8) throw ERROR;
                return right;
            }
        };
        Supplier collection = new Supplier() {
            public Object get() {
                require(Trace.events == 123);
                counts[2]++;
                if (mode == 9) throw RUNTIME;
                return mode == 11 ? replacement : mode == 17 ? null : items;
            }
        };
        Supplier map = new Supplier() {
            public Object get() {
                require(Trace.events == 1234 && counts[2] == 1);
                counts[3]++;
                if (mode == 10) throw ERROR;
                return mode == 12 ? replacement : mode == 18 ? null : index;
            }
        };
        Trace.action = mode == 14 ? null : new Runnable() {
            public void run() {
                require(Trace.events == 12345 && counts[3] == 1);
                counts[4]++;
                if (mode == 13) throw RUNTIME;
                if (mode == 20) Trace.result = other;
            }
        };
        if (mode == 1) first = null;
        if (mode == 2) second = null;
        if (mode == 3) collection = null;
        if (mode == 4) map = null;
        if (mode == 23) second = first;
        Object result = null;
        Throwable failure = null;
        try { result = DependentCollectorCaller.call(route, first, second, collection, map); }
        catch (Throwable caught) { failure = caught; }
        int stage = mode == 1 || mode == 5 || mode == 6 || mode == 22 ? 1
                : mode == 2 || mode == 7 || mode == 8 ? 2
                : mode == 3 || mode == 9 || mode == 11 ? 3
                : mode == 4 || mode == 10 || mode == 12 ? 4
                : mode == 13 || mode == 14 ? 5 : 6;
        long events = 0;
        for (int digit = 1; digit <= stage; digit++) events = events * 10 + digit;
        require(Trace.targets == 1 && Trace.events == events && Trace.seen == source);
        require(Trace.first == first && Trace.second == second);
        require(Trace.collectionSupplier == collection && Trace.mapSupplier == map);
        require(counts[0] == (mode == 1 || mode == 22 ? 0 : mode == 23 ? 2 : 1));
        require(counts[1] == (stage >= 2 && mode != 2 && mode != 23 ? 1 : 0));
        require(counts[2] == (stage >= 3 && mode != 3 ? 1 : 0));
        require(counts[3] == (stage >= 4 && mode != 4 ? 1 : 0));
        require(counts[4] == (stage >= 5 && mode != 14 ? 1 : 0));
        if (counts[0] != 0) require(received[0] == source);
        if (counts[1] != 0) require(received[1] == source);
        require(Trace.left == (stage >= 2 ? left : null));
        require(Trace.right == (stage >= 3 ? mode == 23 ? left : right : null));
        require(Trace.collection == (stage >= 4 && mode != 17 ? items : null));
        require(Trace.map == (stage >= 5 && mode != 18 ? index : null));
        if (mode == 5 || mode == 7 || mode == 9 || mode == 13) require(failure == RUNTIME);
        else if (mode == 6 || mode == 8 || mode == 10) require(failure == ERROR);
        else if (mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 14)
            require(failure instanceof NullPointerException);
        else if (mode == 11 || mode == 12 || mode == 15 || mode == 22)
            require(failure instanceof ClassCastException);
        else {
            require(failure == null);
            require(result == (mode == 20 ? other : mode == 16 ? null : original));
        }
        require(Trace.source == (mode == 19 ? replacement : source));
        require(Trace.result == (mode == 20 ? other : mode == 15 ? replacement : mode == 16 ? null : original));
        require(items.size() == 1 && items.iterator().next() == left);
        require(index.size() == 1 && index.get(left) == items);
        System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        run(route, mode, 0);
        run(route, mode, 1);
    }
}
