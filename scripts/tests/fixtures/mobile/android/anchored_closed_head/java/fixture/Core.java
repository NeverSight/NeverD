package fixture;
import java.util.Map;
public final class Core {
    private Core() {}
    public static Map call(Context first, Class type, String label, Context last, Producer producer) {
        Factory.events = Factory.events * 5 + 3;
        Map result = Factory.create(first, type, label, last, producer);
        Factory.events = Factory.events * 5 + 4;
        return result;
    }
    public static void discard(Context first, Class type, String label, Context last, Producer producer) {
        call(first, type, label, last, producer);
    }
    public static <R> Map<Object, R> typed(Context first, Class<R> type, String label,
            Context last, Producer<? extends R> producer) {
        Factory.events = Factory.events * 5 + 3;
        Map<Object, R> result = Factory.create(first, type, label, last, producer);
        Factory.events = Factory.events * 5 + 4;
        return result;
    }
}
