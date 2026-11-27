import java.util.List;
import java.util.function.IntUnaryOperator;

public class Features<T extends Number> {
    private final List<T> values;
    public Features(List<T> values) { this.values = values; }
    public T first() { return values.get(0); }
    public static IntUnaryOperator add(int step) { return x -> x + step; }
    public static String describe(int n) { return "value=" + n; }
    public static Class<?> lookup(String n) throws Exception { return Class.forName(n); }
    public static synchronized int locked(int n) { return n + 1; }
    public static void cleanup(List<String> log) {
        try { log.add("body"); } finally { log.add("cleanup"); }
    }
}
