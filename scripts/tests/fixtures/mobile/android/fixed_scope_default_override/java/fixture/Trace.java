package fixture;
public final class Trace {
    public static int calls;
    public static Object last;
    public static RuntimeException failure;
    public static <T> T echo(T value) {
        ++calls;
        last = value;
        if (failure != null) throw failure;
        return value;
    }
}
