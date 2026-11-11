package fixture;
@SuppressWarnings("unchecked")
public class Provider implements Api {
    public Object stored, seen;
    public RuntimeException failure;
    public int calls, index;
    public long stamp;
    public double fraction;
    public static Object staticStored, staticSeen;
    public static RuntimeException staticFailure;
    public static int staticCalls, staticIndex;
    public static long staticStamp;
    public static double staticFraction;
    public <T extends Value> Carrier<T> read(Object token, int next, long word, double bits) {
        calls++; seen = token; index = next; stamp = word; fraction = bits;
        if (failure != null) throw failure;
        return (Carrier<T>) stored;
    }
    public static <T extends Box> Carrier<T> staticRead(Object token, int next, long word, double bits) {
        staticCalls++; staticSeen = token; staticIndex = next; staticStamp = word; staticFraction = bits;
        if (staticFailure != null) throw staticFailure;
        return (Carrier<T>) staticStored;
    }
}
