package fixture;
public final class Trace {
    public static long events;
    public static int failAt,value,next,delta,calls;
    public static RuntimeException failure;
    public static Object seen;
    public static Inner partial;
    public static StackTraceElement[] firstFrames,lastFrames;
    public static java.util.function.IntConsumer observer;
    public static void hit(int event) { events=events*10+event; if(observer!=null)observer.accept(event); if(event==failAt)throw failure; }
}
