package fixture;
public class Trace {
    public static int events, failAt, baseCalls, nextWord;
    public static long nextWide;
    public static RuntimeException failure;
    public static void hit(int step) {
        events = events * 10 + step;
        if (step == failAt) throw failure;
    }
}
