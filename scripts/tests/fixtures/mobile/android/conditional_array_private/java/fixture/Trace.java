package fixture;
public final class Trace {
    public static int events, mask, failAt, seenIndex, seenNext;
    public static int replacementValue;
    public static long seenFirst, seenBranch;
    public static Object seenArray, seenKey, seenReturn, replacement;
    public static int[] afterValues, afterFlags;
    public static Target seenTarget, result;
    public static Throwable failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == failAt) {
            if (failure instanceof RuntimeException) throw (RuntimeException) failure;
            throw (Error) failure;
        }
    }
}
