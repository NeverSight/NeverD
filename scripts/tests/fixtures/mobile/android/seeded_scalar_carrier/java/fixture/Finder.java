package fixture;
import java.util.Collections;
import java.util.List;
public class Finder {
    static { Trace.hit(4); }
    public static <T> int find(List<? extends Comparable<? super T>> values, T needle) {
        Trace.before(values, needle);
        int result = Collections.binarySearch(values, needle);
        Trace.after(values, needle, result);
        return result;
    }
}
