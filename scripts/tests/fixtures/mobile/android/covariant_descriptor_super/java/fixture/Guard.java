package fixture;
public class Guard {
    public static int calls;
    public static Object last;
    public static RuntimeException failure;
    public static Error error;
    public static void check(Object value) {
        calls++;
        last = value;
        if (failure != null) throw failure;
        if (error != null) throw error;
        if (value == null) throw new NullPointerException();
    }
}
