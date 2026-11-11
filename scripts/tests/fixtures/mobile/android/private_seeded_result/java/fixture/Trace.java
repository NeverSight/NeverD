package fixture;
public final class Trace {
    public static long events;
    public static int failAt,callbacks,productions;
    public static RuntimeException failure;
    public static Object seenReceiver,seenSeed,seenCallback,saved,value,next,extra,callbackValue;
    public static Token token;
    public static java.util.function.IntConsumer observer;
    public static void hit(int event) {
        events=events*10+event;
        if(observer!=null)observer.accept(event);
        if(event==failAt)throw failure;
    }
}
