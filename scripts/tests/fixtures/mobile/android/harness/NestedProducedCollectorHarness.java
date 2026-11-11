import fixture.Trace;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.LinkedHashMap;
import java.util.Set;
import java.util.function.Supplier;
import java.util.function.BiConsumer;
import java.util.function.BinaryOperator;
import java.util.function.Function;
import java.util.stream.Collector;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class NestedProducedCollectorHarness {
    private static final RuntimeException RUNTIME = new RuntimeException("owned supplier failure");
    private static final Error ERROR = new AssertionError("owned supplier error");
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned nested produced collector mismatch");
    }
    private static Collector collector(final int which, final int mode, final int[] calls,
            final Map index, final Object key) {
        return new Collector<Map, Map, Map>() {
            public Supplier<Map> supplier() {
                require(Trace.events == 123);
                return new Supplier<Map>() {
                    public Map get() {
                        require(Trace.events == 123); calls[3 + which]++; return index;
                    }
                };
            }
            public BiConsumer<Map, Map> accumulator() {
                require(Trace.events == 123);
                return new BiConsumer<Map, Map>() {
                    public void accept(Map output, Map input) {
                        require(output == index && Trace.events == 123); calls[5]++;
                        if (mode == 16) throw RUNTIME;
                        if (mode == 17) throw ERROR;
                        String value = (String)input.get(key);
                        List values = (List)output.get(key);
                        values.add(value);
                    }
                };
            }
            public Function<Map, Map> finisher() {
                require(Trace.events == 123);
                return new Function<Map, Map>() {
                    public Map apply(Map value) {
                        require(value == index && Trace.events == 123); calls[6]++; return value;
                    }
                };
            }
            public BinaryOperator<Map> combiner() { throw new AssertionError("owned unexpected combine"); }
            public Set<Collector.Characteristics> characteristics() { throw new AssertionError("owned unexpected characteristics"); }
        };
    }
    private static void run(int route, final int mode, int attempt) {
        final Object key = new Object(), wrong = new Object(), item = new Object();
        final String value = new String("owned element");
        final List seedValues = new ArrayList(), firstValues = new ArrayList(), secondValues = new ArrayList();
        seedValues.add(item); firstValues.add(item); secondValues.add(item);
        final Map seed = new LinkedHashMap(), first = new LinkedHashMap(), second = new LinkedHashMap(), input = new LinkedHashMap();
        seed.put(key, seedValues);
        first.put(key, mode == 18 ? wrong : mode == 20 ? null : firstValues);
        second.put(key, secondValues);
        input.put(key, mode == 19 ? wrong : value);
        final int[] calls = new int[7];
        final Collector original = collector(0, mode, calls, first, key), other = collector(1, mode, calls, second, key);
        Trace.targets = 0; Trace.events = 0; Trace.supplier = null; Trace.seed = null;
        Trace.result = mode == 9 ? wrong : mode == 10 ? null : original;
        final Runnable alternate = new Runnable() {
            public void run() { require(Trace.events == 12); calls[2]++; }
        };
        Trace.action = mode == 8 ? null : new Runnable() {
            public void run() {
                require(Trace.events == 12); calls[1]++;
                if (mode == 6) throw RUNTIME;
                if (mode == 7) throw ERROR;
                if (mode == 11) Trace.result = other;
            }
        };
        Supplier supplier = mode == 1 ? null : new Supplier() {
            public Object get() {
                require(Trace.events == 1 && Trace.targets == 1 && Trace.seed == null);
                calls[0]++;
                if (mode == 2) throw RUNTIME;
                if (mode == 3) throw ERROR;
                if (mode == 12) Trace.result = other;
                if (mode == 13) Trace.seed = wrong;
                if (mode == 14) Trace.action = alternate;
                if (mode == 15) Trace.result = wrong;
                return mode == 4 ? wrong : mode == 5 ? null : seed;
            }
        };
        Object result = null; Throwable failure = null;
        try { result = NestedProducedCollectorCaller.call(route, supplier); }
        catch (Throwable caught) { failure = caught; }
        int stage = mode >= 1 && mode <= 3 ? 1 : mode >= 6 && mode <= 8 ? 2 : 3;
        require(Trace.targets == 1 && Trace.events == (stage == 1 ? 1 : stage == 2 ? 12 : 123));
        require(Trace.supplier == supplier && calls[0] == (mode == 1 ? 0 : 1));
        require(calls[1] == (stage >= 2 && mode != 8 && mode != 14 ? 1 : 0));
        require(calls[2] == (mode == 14 ? 1 : 0));
        require(Trace.seed == (stage == 1 || mode == 5 ? null : mode == 4 ? wrong : seed));
        boolean changed = mode == 11 || mode == 12;
        Object expected = mode == 9 || mode == 15 ? wrong : mode == 10 ? null : changed ? other : original;
        require(Trace.result == expected && calls[3] == 0 && calls[4] == 0 && calls[5] == 0 && calls[6] == 0);
        if (mode == 2 || mode == 6) require(failure == RUNTIME);
        else if (mode == 3 || mode == 7) require(failure == ERROR);
        else if (mode == 1 || mode == 8) require(failure instanceof NullPointerException);
        else if (mode == 9 || mode == 15) require(failure instanceof ClassCastException);
        else require(failure == null && result == expected);
        boolean later = failure == null && result != null;
        if (later) {
            Object state = null, finished = null; Throwable deferred = null;
            try {
                Collector selected = (Collector)result;
                state = selected.supplier().get();
                selected.accumulator().accept(mode == 21 ? wrong : state, mode == 22 ? wrong : mode == 23 ? null : input);
                finished = selected.finisher().apply(state);
            } catch (Throwable caught) { deferred = caught; }
            require(state == (changed ? second : first));
            if (mode == 16) require(deferred == RUNTIME);
            else if (mode == 17) require(deferred == ERROR);
            else if (mode == 18 || mode == 19 || mode == 21 || mode == 22) require(deferred instanceof ClassCastException);
            else if (mode == 20 || mode == 23) require(deferred instanceof NullPointerException);
            else require(deferred == null && finished == state);
        }
        require(calls[3] == (later && !changed ? 1 : 0) && calls[4] == (later && changed ? 1 : 0));
        require(calls[5] == (later && mode != 21 && mode != 22 ? 1 : 0));
        boolean completed = later && mode < 16;
        require(calls[6] == (completed ? 1 : 0));
        require(firstValues.size() == (completed && !changed ? 2 : 1));
        require(secondValues.size() == (completed && changed ? 2 : 1));
        require(firstValues.get(0) == item && secondValues.get(0) == item);
        if (completed) require(((List)(changed ? second : first).get(key)).get(1) == value);
        require(seed.size() == 1 && seed.get(key) == seedValues && seedValues.size() == 1 && seedValues.get(0) == item);
        require(first.size() == 1 && second.size() == 1 && input.size() == 1 && input.get(key) == (mode == 19 ? wrong : value));
        System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        run(route, mode, 0); run(route, mode, 1);
    }
}
