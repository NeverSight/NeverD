package fixture;
import java.util.Collection;
public final class Core {
    private Core() {}
    public static <Value extends Comparable<? super Value>> Value call(Collection<Value> values) {
        return Provider.select(values);
    }
    public static Comparable checked(Object values) {
        return call((Collection)values);
    }
    public static String narrow(Collection<String> values) {
        return call(values);
    }
}
