package fixture;

public final class Box {
    public long value;
    public static int calls;
    public static long lastInput;
    public static Object marker;
    public static Object observed;
    public static RuntimeException failure;
    public static Box cached;
    public static Box last;
    public static boolean returnNull;

    private Box(long value) { this.value = value; }
    public static Box box(long value) {
        calls++;
        lastInput = value;
        observed = marker;
        if (marker instanceof int[]) ((int[]) marker)[0] += 5;
        if (failure != null) throw failure;
        if (returnNull) last = null;
        else if (cached != null) last = cached;
        else last = new Box(value);
        return last;
    }
}
