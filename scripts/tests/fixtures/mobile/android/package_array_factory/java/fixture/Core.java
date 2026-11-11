package fixture;
import java.util.List;
public final class Core {
    private Core() {}
    public static <T> List<T> make(T[] values) {
        Trace.hit(3);
        List<T> result = Provider.make(values);
        Trace.hit(4);
        return result;
    }
    public static String narrowed(Object[] values) {
        List<?> result = make(values);
        return (String) result.get(Trace.index);
    }
    public static void ignored(Object[] values) { make(values); }
}
