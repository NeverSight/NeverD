package fixture;
import java.util.Map;
public final class Factory {
    public static int events;
    public static Context savedContext, savedTail;
    public static Class<?> savedType;
    public static String name, savedLabel;
    public static Producer pending;
    public static Object produced, saved;
    public static Map replacement;
    public static RuntimeException failure;
    private Factory() {}
    public static <T> Map<Object, T> create(Context first, Class<T> type, String label,
            Context last, Producer<? extends T> producer) {
        events = events * 5 + 1;
        savedContext = first; savedType = type; first.touch();
        name = type.getName(); savedTail = last; last.touch();
        savedLabel = label; pending = producer;
        produced = producer.next();
        if (failure != null) throw failure;
        saved = replacement;
        events = events * 5 + 2;
        return replacement;
    }
}
