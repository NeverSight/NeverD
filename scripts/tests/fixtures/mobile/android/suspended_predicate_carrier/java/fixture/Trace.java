package fixture;
public final class Trace {
    public static long events;
    public static int mask;
    public static java.util.function.IntConsumer observer;
    public static Object seenInput, seenValue, seenReturn, seenOutput;
    public static Completion<?> seenCompletion;
    public static Boolean seenDone, done;
    public static Completion<? super Boolean> completion;
    public static Object callbackResult;
    public static Carrier<?> output;
    public static void hit(int event) {
        events = events * 10 + event;
        if (observer != null) observer.accept(event);
    }
}
