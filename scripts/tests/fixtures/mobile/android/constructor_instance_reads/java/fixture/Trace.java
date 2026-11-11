package fixture;
public class Trace {
    public static boolean active;
    public static long events;
    public static int fail;
    public static Throwable failure;
    public static Core getter, partial, current, other;
    public static Values afterGetter, afterTarget;
    public static String name, changedName;
    public static Object[] replacement, otherReplacement;
    public static Object context, otherContext;
    public static void step(int tag) {
        if (!active) return;
        events=events*10+tag;
        if (tag==2) { name=changedName;current=other;replacement=otherReplacement;context=otherContext; }
        if (tag==3) { getter.set(afterTarget);afterGetter.assign(afterTarget); }
        if (fail==tag) {
            if (failure instanceof RuntimeException) throw (RuntimeException)failure;
            throw (Error)failure;
        }
    }
}
