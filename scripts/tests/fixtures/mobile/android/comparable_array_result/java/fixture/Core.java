package fixture;
import java.util.Comparator;
public final class Core {
    private Core() {}
    public static <Value extends Comparable<? super Value>> Value single(Value[] values) {
        return Provider.one(values);
    }
    public static <Value extends Comparable<? super Value>> Value configured(Value[] values, Comparator<Value> comparator) {
        return Provider.pick(values, comparator);
    }
    public static Comparable checked(Object values, Object comparator) {
        return configured((Comparable[]) values, (Comparator) comparator);
    }
}
