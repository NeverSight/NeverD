package fixture;
public class Trace {
    public static int events, failAt, baseCalls, later, initialWord, nextWord;
    public static long initialWide, nextWide;
    public static Base observed;
    public static Owner captured;
    public static RuntimeException failure;
    public static void hit(int step) {
        events = events * 10 + step;
        if (step == 3) {
            Provider.word = nextWord;
            Provider.wide = nextWide;
        }
        if (step == failAt) throw failure;
    }
}
