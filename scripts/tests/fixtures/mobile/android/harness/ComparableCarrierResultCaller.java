package fixture;
import java.util.Collection;
public final class ComparableCarrierResultCaller {
    public static <Cell extends Comparable<? super Cell>> Cell call(Collection<Cell> values) { return Core.call(values); }
    public static Comparable checked(Object values) { return Core.checked(values); }
    public static String narrow(Collection<String> values) { return Core.narrow(values); }
}
