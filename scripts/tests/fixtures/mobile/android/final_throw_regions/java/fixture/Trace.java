package fixture;
public final class Trace {
    public static int mode;
    public static long events;
    public static RuntimeException failure;
    public static Object value;
    public static Core escaped;
    public static boolean inspectStatic;
    public static Object observedFirst;
    public static Object observedSecond;
    public static void event(int value) {
        events = events * 10 + value;
        if (inspectStatic && value == 2) {
            observedFirst = StaticCore.first;
            observedSecond = StaticCore.second;
        }
    }
    public static void __neverdThrow() {}
}
