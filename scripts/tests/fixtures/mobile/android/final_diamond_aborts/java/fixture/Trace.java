package fixture;
public class Trace {
    public static Core partial;
    public static long events;
    public static volatile long stamp;
    public static long changedStamp, otherStamp;
    public static boolean accepted, inlineAbort, rejectAlternate, abortAfter;
    public static int fail, accepts, chooses, finishes, problems;
    public static Throwable failure;
    public static RuntimeException problemFailure;
    public static Object produced, finalValue, replacement;
    public static Object acceptedSeed, chosenSeed, finishedSeed;
    public static void step(int tag) {
        events=(events<<4)|tag;
        if (tag==8) produced=replacement;
        if (tag==11) finalValue=replacement;
        if (tag==12) stamp=changedStamp;
        if (tag==13) stamp=otherStamp;
        if (fail==tag) {
            if (failure instanceof RuntimeException) throw (RuntimeException)failure;
            throw (Error)failure;
        }
    }
    public static boolean accept(Object seed) {
        accepts++;acceptedSeed=seed;
        step(3);
        return accepted;
    }
    public static Object choose(Object seed) {
        chooses++;chosenSeed=seed;
        Object snapshot=produced;
        step(8);
        return snapshot;
    }
    public static Object finish(Object seed) {
        finishes++;finishedSeed=seed;
        Object snapshot=finalValue;
        step(11);
        return snapshot;
    }
    public static RuntimeException problem(int tag) {
        problems++;
        step(tag);
        return problemFailure;
    }
}
