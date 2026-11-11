package fixture;
import java.util.Collections;
import java.util.List;
public class Sorter {
    static { Trace.hit(4); }
    public static <T extends Comparable<? super T>> void sort(List<T> values) {
        Trace.before(values);
        Collections.sort(values);
        Trace.after(values);
    }
}
