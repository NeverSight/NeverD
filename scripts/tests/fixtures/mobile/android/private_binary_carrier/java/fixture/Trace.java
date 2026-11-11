package fixture;
public final class Trace {
    private Trace() {}
    public static long events;
    public static int failAt,calls;
    public static RuntimeException failure;
    public static Object receiver,left,right,first,second,result;
    public static java.util.function.IntConsumer observer;
    public static void hit(int event) {
        events=events*10+event;
        if(observer!=null)observer.accept(event);
        if(failAt==event)throw failure;
    }
}
