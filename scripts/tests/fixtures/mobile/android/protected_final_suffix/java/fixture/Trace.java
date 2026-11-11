package fixture;
public class Trace {
    public static int mode;
    public static long events, stamp, observedStamp;
    public static Object first, second, replacementFirst, replacementSecond;
    public static Object observedFirst, observedSecond;
    public static boolean observedComplete;
    public static Throwable caught;
    public static RuntimeException failure, secondary;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == 7) {
            observedFirst = Core.first; observedSecond = Core.second;
            observedStamp = Core.stamp; observedComplete = Core.complete;
        }
        if (mode == 13) {
            if (event == 1) { first = replacementFirst; second = replacementSecond; }
            if (event == 3) stamp = 99;
        }
        if ((event == 1 && (mode == 1 || mode == 3 || mode == 5))
                || (event == 2 && (mode == 2 || mode == 4 || mode == 6 || mode == 8 || mode == 9))
                || (event == 3 && (mode == 11 || mode == 12))
                || (event == 6 && mode == 7)) throw failure;
        if ((event == 4 && mode == 8) || (event == 5 && mode == 9)
                || (event == 7 && mode == 10)) throw secondary;
    }
}
