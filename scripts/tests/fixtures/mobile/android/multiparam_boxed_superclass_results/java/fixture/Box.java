package fixture;
public final class Box {
    public byte[] value;
    public static int calls;
    public static byte[] lastInput;
    public static Object marker, observed;
    public static RuntimeException failure;
    public static Box cached, last;
    public static boolean returnNull;
    public Box(byte[] value) { this.value = value; }
    public static Box box(byte[] value) {
        calls++; lastInput = value; observed = marker;
        if (marker instanceof int[]) ((int[])marker)[0] += 5;
        if (failure != null) throw failure;
        if (returnNull) last = null;
        else if (cached != null) last = cached;
        else last = new Box(value);
        return last;
    }
}
