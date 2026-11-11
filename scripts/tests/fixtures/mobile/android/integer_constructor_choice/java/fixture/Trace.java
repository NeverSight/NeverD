package fixture;
public final class Trace {
    public static long events;
    public static int failAt,constructors,seed,next,argument;
    public static RuntimeException failure;
    public static Throwable caught;
    public static Widget current;
    public static Object received;
    public static java.util.function.IntConsumer observer;
    public static void hit(int event) {
        events=events*10+event;
        if(observer!=null)observer.accept(event);
        if(event==failAt)throw failure;
    }
}
