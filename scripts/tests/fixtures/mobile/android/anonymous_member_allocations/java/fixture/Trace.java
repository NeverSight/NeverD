package fixture;
public class Trace {
    public static int events, failAt;
    public static boolean choose;
    public static String firstName, secondName, replacement;
    public static Object constructed;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == 3) { Names.first = replacement; Names.second = replacement; }
        if (event == failAt) throw failure;
    }
}
