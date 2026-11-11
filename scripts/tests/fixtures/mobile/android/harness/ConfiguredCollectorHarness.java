import fixture.Trace;
import java.util.ArrayList;
import java.util.function.Function;
import java.util.stream.Collector;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class ConfiguredCollectorHarness {
    private static final RuntimeException RUNTIME = new RuntimeException("owned callback failure");
    private static final Error ERROR = new AssertionError("owned callback error");
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned configured collector mismatch");
    }
    private static Collector collector() {
        return Collector.of(ArrayList::new, ArrayList::add,
                (ArrayList a, ArrayList b) -> { a.addAll(b); return a; });
    }
    private static final class Text implements CharSequence {
        private final int id, mode;
        private final int[] counts;
        private final Object replacement;
        Text(int id, int mode, int[] counts, Object replacement) {
            this.id = id; this.mode = mode; this.counts = counts; this.replacement = replacement;
        }
        public int length() {
            int slot = Trace.events == 12 ? 0 : Trace.events == 123 ? 1 : Trace.events == 1234 ? 2 : -1;
            require(slot >= 0 && (id == slot || mode == 23) && counts[0] == 1);
            counts[slot + 1]++;
            if (mode == 9 && slot == 0 || mode == 13 && slot == 2) throw RUNTIME;
            if (mode == 11 && slot == 1) throw ERROR;
            if (mode == 21 && slot == 0) Trace.result = replacement;
            return mode == 22 && slot == 2 ? -7 : id == 0 ? 2 : id == 1 ? 3 : 5;
        }
        public char charAt(int index) { throw new AssertionError("owned configured collector unexpected character"); }
        public CharSequence subSequence(int start, int end) { throw new AssertionError("owned configured collector unexpected subsequence"); }
        public String toString() { throw new AssertionError("owned configured collector unexpected rendering"); }
    }
    private static void run(int route, final int mode, int attempt) {
        final Object source = mode == 4 ? Integer.valueOf(37) : mode == 5 ? null : new String("owned source");
        final Object replacement = new Object();
        final int[] counts = new int[5];
        final Object[] received = new Object[1];
        final Collector original = collector(), other = collector();
        final CharSequence separator = mode == 8 ? null : new Text(0, mode, counts, replacement);
        final CharSequence prefix = mode == 10 ? null : mode == 23 ? separator : new Text(1, mode, counts, replacement);
        final CharSequence suffix = mode == 12 ? null : mode == 23 ? separator : new Text(2, mode, counts, replacement);
        final CharSequence mapped = mode == 23 ? separator : new StringBuilder("owned mapped");
        final CharSequence newConfig = new StringBuilder("owned replacement");
        Trace.targets = Trace.shortCalls = 0;
        Trace.events = 0;
        Trace.source = source;
        Trace.seen = Trace.mapper = null;
        Trace.separator = Trace.prefix = Trace.suffix = Trace.mapped = null;
        Trace.prefixConfig = prefix;
        Trace.suffixConfig = suffix;
        Trace.separatorLength = Trace.prefixLength = Trace.suffixLength = -1;
        Trace.result = mode == 16 ? replacement : mode == 17 ? null : original;
        Function mapper = new Function<String, Object>() {
            public Object apply(String input) {
                require(Trace.events == 1 && input == source);
                counts[0]++;
                received[0] = input;
                if (mode == 2) throw RUNTIME;
                if (mode == 3) throw ERROR;
                if (mode == 19) Trace.result = other;
                if (mode == 20) { Trace.prefixConfig = newConfig; Trace.suffixConfig = newConfig; }
                return mode == 6 ? replacement : mode == 7 ? null : mapped;
            }
        };
        Trace.action = mode == 14 ? null : new Runnable() {
            public void run() {
                require(Trace.events == 12345 && counts[3] == 1);
                counts[4]++;
                if (mode == 15) throw RUNTIME;
                if (mode == 18) Trace.result = other;
            }
        };
        if (mode == 1) mapper = null;
        Object result = null;
        Throwable failure = null;
        try { result = ConfiguredCollectorCaller.call(route, separator, prefix, suffix, mapper); }
        catch (Throwable caught) { failure = caught; }
        int stage = mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 6 ? 1
                : mode == 8 || mode == 9 ? 2 : mode == 10 || mode == 11 ? 3
                : mode == 12 || mode == 13 ? 4 : mode == 14 || mode == 15 ? 5 : 6;
        long events = 0;
        for (int digit = 1; digit <= stage; digit++) events = events * 10 + digit;
        require(Trace.targets == 1 && Trace.shortCalls == (route < 2 ? 1 : 0));
        require(Trace.events == events && Trace.seen == source && Trace.mapper == mapper);
        require(Trace.separator == separator && Trace.prefix == prefix && Trace.suffix == suffix);
        require(counts[0] == (mode == 1 || mode == 4 ? 0 : 1));
        require(counts[1] == (stage >= 2 && mode != 8 ? 1 : 0));
        require(counts[2] == (stage >= 3 && mode != 10 ? 1 : 0));
        require(counts[3] == (stage >= 4 && mode != 12 ? 1 : 0));
        require(counts[4] == (stage >= 5 && mode != 14 ? 1 : 0));
        if (counts[0] != 0) require(received[0] == source);
        require(Trace.mapped == (stage >= 2 && mode != 7 ? mapped : null));
        require(Trace.separatorLength == (stage >= 3 ? 2 : -1));
        require(Trace.prefixLength == (stage >= 4 ? mode == 23 ? 2 : 3 : -1));
        require(Trace.suffixLength == (stage >= 5 ? mode == 22 ? -7 : mode == 23 ? 2 : 5 : -1));
        if (mode == 2 || mode == 9 || mode == 13 || mode == 15) require(failure == RUNTIME);
        else if (mode == 3 || mode == 11) require(failure == ERROR);
        else if (mode == 1 || mode == 8 || mode == 10 || mode == 12 || mode == 14)
            require(failure instanceof NullPointerException);
        else if (mode == 4 || mode == 6 || mode == 16 || mode == 21)
            require(failure instanceof ClassCastException);
        else {
            require(failure == null);
            require(result == (mode == 18 || mode == 19 ? other : mode == 17 ? null : original));
        }
        require(Trace.result == (mode == 16 || mode == 21 ? replacement : mode == 17 ? null : mode == 18 || mode == 19 ? other : original));
        require(Trace.prefixConfig == (mode == 20 ? newConfig : prefix));
        require(Trace.suffixConfig == (mode == 20 ? newConfig : suffix));
        System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        run(route, mode, 0);
        run(route, mode, 1);
    }
}
