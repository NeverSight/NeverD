package fixture;
import java.util.Comparator;
import java.util.function.Function;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core {
    private Core() {}
    public static Comparator raw(Function selector) {
        return Factory.create(selector);
    }
    public static void ignore(Function selector) {
        Factory.create(selector);
    }
    public static <E,K extends Comparable<? super K>> Comparator<E> relay(Function<? super E,? extends K> selector) {
        return Factory.create(selector);
    }
    public static Comparator standard(Function selector) {
        return Comparator.comparing(selector);
    }
    public static void ignoreStandard(Function selector) {
        Comparator.comparing(selector);
    }
}
