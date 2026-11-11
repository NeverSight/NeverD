package fixture;
public final class Carrier implements Comparable<Carrier> {
    public long value;
    public int calls;
    public int order;
    public static int reads;
    public RuntimeException failure;
    public static int factories;
    public static RuntimeException factoryFailure;
    public static long factoryInput;
    public static Carrier last;
    public Carrier(long value) { this.value = value; }
    public long read() {
        calls++; order = ++reads;
        if (failure != null) throw failure;
        return value;
    }
    public static Carrier make(long value) {
        factories++; factoryInput = value;
        if (factoryFailure != null) throw factoryFailure;
        Carrier result = new Carrier(value); last = result; return result;
    }
    public int compareTo(Carrier other) { return value < other.value ? -1 : value == other.value ? 0 : 1; }
}
