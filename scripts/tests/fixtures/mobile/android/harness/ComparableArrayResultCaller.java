package fixture;
import java.util.Comparator;
public final class ComparableArrayResultCaller {
    public static <Cell extends Comparable<? super Cell>> Cell single(Cell[] values) { return Core.single(values); }
    public static <Cell extends Comparable<? super Cell>> Cell configured(Cell[] values, Comparator<Cell> comparator) { return Core.configured(values, comparator); }
    public static Comparable checked(Object values, Object comparator) { return Core.checked(values, comparator); }
}
