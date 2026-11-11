package fixture;
public final class Trace {
    private Trace() {}
    public static long events,word,observedWord;
    public static int failAt;
    public static Core partial;
    public static Object firstValue,lastValue,observedInput;
    public static RuntimeException failure;
    public static java.util.function.IntConsumer observer;
    public static void hit(int event) {
        events=events*10+event;
        if(observer!=null)observer.accept(event);
        if(event==failAt)throw failure;
    }
}
