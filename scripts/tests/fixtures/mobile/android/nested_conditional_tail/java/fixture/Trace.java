package fixture;
import java.util.function.IntConsumer;
public final class Trace {
    public static Provider provider;
    public static Object outerPayload, innerPayload, fallbackValue, changedPayload, changedExtra;
    public static Item item;
    public static Envelope outer;
    public static long trace, inputBits;
    public static volatile long tailBits;
    public static int fallbackCalls, tailCalls, failAt;
    public static IntConsumer observer;
    public static RuntimeException failure;
    public static Error fatal;
    public static void step(int value) {
        trace = trace * 16 + value;
        if (observer != null) observer.accept(value);
        if (value == failAt) {
            if (fatal != null) throw fatal;
            throw failure;
        }
    }
}
