import fixture.Trace;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;
import java.util.function.BiConsumer;
import java.util.function.BinaryOperator;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.stream.Collector;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class GroupedCollectorHarness {
    private static final RuntimeException RUNTIME = new RuntimeException("owned callback failure");
    private static final Error ERROR = new AssertionError("owned callback error");
    private static final class State {}
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned grouped collector mismatch");
    }
    private static Collector collector() {
        return Collector.of(ArrayList::new, ArrayList::add,
                (ArrayList a, ArrayList b) -> { a.addAll(b); return a; });
    }
    private static void run(int route, final int mode, int attempt) {
        final Object source = mode == 12 ? null : mode == 22 ? Integer.valueOf(37) : new String("owned source");
        final Object replacement = new Object(), key = new Object(), finished = new Object();
        final State state = new State();
        final Collector original = collector(), other = collector();
        final Map index = new LinkedHashMap();
        index.put(key, finished);
        final int[] counts = new int[7];
        final Object[] received = new Object[2];
        Trace.targets = 0;
        Trace.events = 0;
        Trace.source = source;
        Trace.seen = Trace.key = Trace.state = Trace.value = null;
        Trace.classifier = Trace.mapSupplier = Trace.downstream = null;
        Trace.map = null;
        Trace.result = mode == 18 ? replacement : mode == 19 ? null : original;
        Function classifier = new Function<String, Object>() {
            public Object apply(String input) {
                require(Trace.events == 1);
                counts[0]++;
                received[0] = input;
                if (mode == 4) throw RUNTIME;
                if (mode == 5) throw ERROR;
                if (mode == 21) Trace.source = replacement;
                return key;
            }
        };
        Supplier map = new Supplier() {
            public Object get() {
                require(Trace.events == 12 && counts[0] == 1);
                counts[1]++;
                if (mode == 6) throw RUNTIME;
                return mode == 7 ? replacement : mode == 19 ? null : index;
            }
        };
        Collector downstream = new Collector<String, State, Object>() {
            public Supplier<State> supplier() {
                require(Trace.events == 123 && counts[1] == 1);
                counts[2]++;
                if (mode == 8) throw RUNTIME;
                if (mode == 9) return null;
                return (Supplier)new Supplier() {
                    public Object get() {
                        require(Trace.events == 123 && counts[2] == 1);
                        counts[3]++;
                        if (mode == 10) throw ERROR;
                        return mode == 11 ? replacement : mode == 12 ? null : state;
                    }
                };
            }
            public Function<State, Object> finisher() {
                require(Trace.events == 1234 && counts[3] == 1);
                counts[4]++;
                if (mode == 13) throw ERROR;
                if (mode == 14) return null;
                return new Function<State, Object>() {
                    public Object apply(State input) {
                        require(Trace.events == 1234 && counts[4] == 1);
                        counts[5]++;
                        received[1] = input;
                        require(input == (mode == 12 ? null : state));
                        if (mode == 15) throw RUNTIME;
                        if (mode == 23) Trace.result = replacement;
                        return finished;
                    }
                };
            }
            public BiConsumer<State, String> accumulator() { throw new AssertionError("owned grouped collector unexpected accumulator"); }
            public BinaryOperator<State> combiner() { throw new AssertionError("owned grouped collector unexpected combiner"); }
            public Set<Characteristics> characteristics() { throw new AssertionError("owned grouped collector unexpected characteristics"); }
        };
        Trace.action = mode == 16 ? null : new Runnable() {
            public void run() {
                require(Trace.events == 12345 && counts[5] == 1);
                counts[6]++;
                if (mode == 17) throw RUNTIME;
                if (mode == 20) Trace.result = other;
            }
        };
        if (mode == 1) classifier = null;
        if (mode == 2) map = null;
        if (mode == 3) downstream = null;
        Object result = null;
        Throwable failure = null;
        try { result = GroupedCollectorCaller.call(route, classifier, map, downstream); }
        catch (Throwable caught) { failure = caught; }
        int stage = mode == 1 || mode == 4 || mode == 5 || mode == 22 ? 1
                : mode == 2 || mode == 6 || mode == 7 ? 2
                : mode == 3 || mode == 8 || mode == 9 || mode == 10 ? 3
                : mode == 11 || mode == 13 || mode == 14 || mode == 15 ? 4
                : mode == 16 || mode == 17 ? 5 : 6;
        long events = 0;
        for (int digit = 1; digit <= stage; digit++) events = events * 10 + digit;
        require(Trace.targets == 1 && Trace.events == events && Trace.seen == source);
        require(Trace.classifier == classifier && Trace.mapSupplier == map && Trace.downstream == downstream);
        require(counts[0] == (mode == 1 || mode == 22 ? 0 : 1));
        require(counts[1] == (stage >= 2 && mode != 2 ? 1 : 0));
        require(counts[2] == (stage >= 3 && mode != 3 ? 1 : 0));
        require(counts[3] == (stage >= 3 && mode != 3 && mode != 8 && mode != 9 ? 1 : 0));
        require(counts[4] == (stage >= 4 ? 1 : 0));
        require(counts[5] == (stage >= 4 && mode != 11 && mode != 13 && mode != 14 ? 1 : 0));
        require(counts[6] == (stage >= 5 && mode != 16 ? 1 : 0));
        if (counts[0] != 0) require(received[0] == source);
        if (counts[5] != 0) require(received[1] == (mode == 12 ? null : state));
        require(Trace.key == (stage >= 2 ? key : null));
        require(Trace.map == (stage >= 3 && mode != 19 ? index : null));
        require(Trace.state == (stage >= 4 ? mode == 11 ? replacement : mode == 12 ? null : state : null));
        require(Trace.value == (stage >= 5 ? finished : null));
        if (mode == 4 || mode == 6 || mode == 8 || mode == 15 || mode == 17) require(failure == RUNTIME);
        else if (mode == 5 || mode == 10 || mode == 13) require(failure == ERROR);
        else if (mode == 1 || mode == 2 || mode == 3 || mode == 9 || mode == 14 || mode == 16)
            require(failure instanceof NullPointerException);
        else if (mode == 7 || mode == 11 || mode == 18 || mode == 22 || mode == 23)
            require(failure instanceof ClassCastException);
        else {
            require(failure == null);
            require(result == (mode == 20 ? other : mode == 19 ? null : original));
        }
        require(Trace.source == (mode == 21 ? replacement : source));
        require(Trace.result == (mode == 20 ? other : mode == 18 || mode == 23 ? replacement : mode == 19 ? null : original));
        require(index.size() == 1 && index.get(key) == finished);
        System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        run(route, mode, 0);
        run(route, mode, 1);
    }
}
