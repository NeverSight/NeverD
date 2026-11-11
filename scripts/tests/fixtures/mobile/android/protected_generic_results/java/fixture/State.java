package fixture;
public class State {
    public static int calls;
    public static Class<?> type;
    public static Fn<Throwable, Throwable> value;
    public static Fn<Throwable, Throwable> last;
    public static RuntimeException failure;
}
