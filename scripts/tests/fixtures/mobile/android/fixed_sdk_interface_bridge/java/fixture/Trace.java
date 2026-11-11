package fixture;
public class Trace {
    public static Object self, other, guardSelf;
    public static Runnable action, guardAction;
    public static boolean invoke;
    public static long word, reply, guardArg, observed;
    public static int events, result;
    public static void hit(int stage) { events=events*10+stage; }
}
