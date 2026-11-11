package fixture;
public final class Trace {
    public static long events,a,b;
    public static int mode,failAt;
    public static Core escaped;
    public static RuntimeException failure1,failure2,failure3,observerFailure;
    public static java.util.function.IntConsumer observer;
    public static void event(int event) { events=events*10+event; observer.accept(event); if(event==failAt)throw observerFailure; }
}
