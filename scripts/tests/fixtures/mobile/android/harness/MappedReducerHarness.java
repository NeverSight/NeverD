import fixture.Trace;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.function.Function;
import java.util.function.BinaryOperator;
import java.util.stream.Collector;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class MappedReducerHarness {
    private static final RuntimeException RUNTIME = new RuntimeException("owned reducer failure");
    private static final Error ERROR = new AssertionError("owned reducer error");
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned mapped reducer mismatch");
    }
    private static Collector collector() {
        return Collector.of(ArrayList::new, ArrayList::add,
            (ArrayList a, ArrayList b) -> { a.addAll(b); return a; });
    }
    private static void owned(int route, final int mode, int attempt) {
        final Object wrong = new Object();
        final Object seed = mode == 6 ? null : mode == 7 ? wrong : new String("owned seed");
        final Object source = mode == 5 ? null : mode == 4 ? wrong : new String("owned input");
        final String mapped = new String("owned mapped"), folded = new String("owned folded");
        final Collector original = collector(), other = collector();
        final int[] calls = new int[4];
        Trace.targets = 0; Trace.events = 0; Trace.source = source;
        Trace.seed = Trace.mapper = Trace.operator = Trace.input = Trace.mapped = Trace.folded = null;
        Trace.result = mode == 16 ? wrong : mode == 17 ? null : original;
        final Runnable alternate = new Runnable() {
            public void run() { require(Trace.events == 123); calls[3]++; }
        };
        final Function raw = new Function() {
            public Object apply(Object value) {
                require(Trace.events == 1 && value == source); calls[0]++;
                if (mode == 2) throw RUNTIME;
                if (mode == 3) throw ERROR;
                if (mode == 19) Trace.result = other;
                if (mode == 21) Trace.action = alternate;
                if (mode == 23) { Trace.source = wrong; Trace.operator = wrong; }
                return mode == 8 ? null : mode == 9 ? wrong : mapped;
            }
        };
        Function mapper = mode == 1 ? null : mode == 9 ? raw : new Function<String, String>() {
            public String apply(String value) { return (String)raw.apply(value); }
        };
        BinaryOperator operator = mode == 10 ? null : new BinaryOperator<String>() {
            public String apply(String left, String right) {
                require(Trace.events == 12 && left == seed && right == (mode == 8 ? null : mapped));
                calls[1]++;
                if (mode == 11) throw RUNTIME;
                if (mode == 12) throw ERROR;
                if (mode == 20) Trace.result = other;
                return mode == 22 ? null : folded;
            }
        };
        Trace.action = mode == 13 ? null : new Runnable() {
            public void run() {
                require(Trace.events == 123); calls[2]++;
                if (mode == 14) throw RUNTIME;
                if (mode == 15) throw ERROR;
                if (mode == 18) Trace.result = other;
            }
        };
        Object result = null; Throwable failure = null;
        try { result = MappedReducerCaller.call(route, seed, mapper, operator); }
        catch (Throwable caught) { failure = caught; }
        int stage = mode >= 1 && mode <= 4 ? 1 : mode == 7 || mode >= 9 && mode <= 12 ? 2
            : mode >= 13 && mode <= 15 ? 3 : 4;
        require(Trace.targets == 1 && Trace.events == (stage == 1 ? 1 : stage == 2 ? 12 : stage == 3 ? 123 : 1234));
        require(Trace.seed == seed && Trace.mapper == mapper && Trace.operator == (mode == 23 ? wrong : operator));
        require(Trace.input == source && Trace.source == (mode == 23 ? wrong : source));
        require(calls[0] == (mode == 1 || mode == 4 ? 0 : 1));
        require(calls[1] == (stage >= 2 && mode != 7 && mode != 9 && mode != 10 ? 1 : 0));
        require(calls[2] == (stage >= 3 && mode != 13 && mode != 21 ? 1 : 0) && calls[3] == (mode == 21 ? 1 : 0));
        require(Trace.mapped == (stage == 1 || mode == 8 ? null : mode == 9 ? wrong : mapped));
        require(Trace.folded == (stage >= 3 && mode != 22 ? folded : null));
        Object expected = mode == 16 ? wrong : mode == 17 ? null : mode >= 18 && mode <= 20 ? other : original;
        require(Trace.result == expected);
        if (mode == 2 || mode == 11 || mode == 14) require(failure == RUNTIME);
        else if (mode == 3 || mode == 12 || mode == 15) require(failure == ERROR);
        else if (mode == 1 || mode == 10 || mode == 13) require(failure instanceof NullPointerException);
        else if (mode == 4 || mode == 7 || mode == 9 || mode == 16) require(failure instanceof ClassCastException);
        else require(failure == null && result == expected);
        System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
    }
    private static void standard(final int mode, int attempt) {
        final Object wrong = new Object();
        final String first = new String("owned first"), second = new String("owned second");
        final String mappedFirst = new String("owned mapped first"), mappedSecond = new String("owned mapped second");
        final String foldedFirst = new String("owned folded first"), foldedSecond = new String("owned folded second");
        final Object seed = mode == 9 ? null : mode == 14 ? wrong : new String("owned initial");
        final Object[] elements = mode == 1 || mode == 18 || mode == 19 ? new Object[0]
            : mode == 20 ? new Object[]{first}
            : new Object[]{mode == 4 ? wrong : mode == 10 ? null : first,
                mode == 15 ? wrong : mode == 21 ? first : mode == 22 ? null : second};
        final int[] calls = new int[2];
        final boolean[] collecting = new boolean[1];
        Trace.targets = 0; Trace.events = 0; Trace.source = first;
        Function mapper = mode == 7 || mode == 18 ? null : new Function<String, String>() {
            public String apply(String value) {
                require(collecting[0] && value == elements[calls[0]]);
                calls[0]++; Trace.hit(1);
                if (mode == 2 || mode == 16 && calls[0] == 2) throw RUNTIME;
                if (mode == 3) throw ERROR;
                return mode == 11 && calls[0] == 1 ? null : calls[0] == 1 ? mappedFirst : mappedSecond;
            }
        };
        BinaryOperator operator = mode == 8 || mode == 19 ? null : new BinaryOperator<String>() {
            public String apply(String left, String right) {
                require(collecting[0] && left == (calls[1] == 0 || mode == 23 ? seed : mode == 13 ? null : foldedFirst));
                require(right == (mode == 11 && calls[1] == 0 ? null : calls[1] == 0 ? mappedFirst : mappedSecond));
                calls[1]++; Trace.hit(2);
                if (mode == 5 || mode == 17 && calls[1] == 2) throw RUNTIME;
                if (mode == 6) throw ERROR;
                return mode == 13 ? null : mode == 23 ? (String)seed : calls[1] == 1 ? foldedFirst : foldedSecond;
            }
        };
        Object result = null; Throwable failure = null; Collector target = null;
        try { target = (Collector)MappedReducerCaller.call(3, seed, mapper, operator); }
        catch (Throwable caught) { failure = caught; }
        require(failure == null && target != null && Trace.targets == 0 && Trace.events == 0 && calls[0] == 0 && calls[1] == 0);
        collecting[0] = true;
        try { result = Arrays.asList(elements).stream().collect(target); }
        catch (Throwable caught) { failure = caught; }
        int maps = elements.length == 0 || mode == 4 || mode == 7 ? 0
            : mode == 2 || mode == 3 || mode == 5 || mode == 6 || mode == 8 || mode == 14 || mode == 15 || mode == 20 ? 1 : 2;
        int operators = elements.length == 0 || mode == 2 || mode == 3 || mode == 4 || mode == 7 || mode == 8 || mode == 14 ? 0
            : mode == 5 || mode == 6 || mode == 15 || mode == 16 || mode == 20 ? 1 : 2;
        require(calls[0] == maps && calls[1] == operators);
        require(Trace.events == (maps == 0 ? 0 : maps == 1 ? operators == 0 ? 1 : 12 : operators == 1 ? 121 : 1212));
        if (mode == 2 || mode == 5 || mode == 16 || mode == 17) require(failure == RUNTIME);
        else if (mode == 3 || mode == 6) require(failure == ERROR);
        else if (mode == 4 || mode == 14 || mode == 15) require(failure instanceof ClassCastException);
        else if (mode == 7 || mode == 8) require(failure instanceof NullPointerException);
        else require(failure == null && result == (elements.length == 0 || mode == 23 ? seed : mode == 13 ? null : mode == 20 ? foldedFirst : foldedSecond));
        require(Trace.targets == 0);
        System.out.println("route=3,mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) {
            if (route == 3) standard(mode, attempt); else owned(route, mode, attempt);
        }
    }
}
