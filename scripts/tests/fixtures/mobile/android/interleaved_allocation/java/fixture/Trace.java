package fixture;
import java.util.function.IntConsumer;
public final class Trace {
    public static Provider provider;
    public static Target target;
    public static Object initFirst,initAlternate,outer;
    public static int initFlag,failAt,calls;
    public static long events,input;
    public static volatile long returned;
    public static RuntimeException failure;
    public static Error fatal;
    public static IntConsumer observer;
    public static void step(int stage) {
        events=events*16+stage;
        if(observer!=null)observer.accept(stage);
        if(stage==failAt){if(fatal!=null)throw fatal;throw failure;}
    }
}
