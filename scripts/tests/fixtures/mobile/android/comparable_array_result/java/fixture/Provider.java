package fixture;
import java.util.Comparator;
public class Provider {
    public static <Entry extends Comparable<? super Entry>> Entry one(Entry[] values) {
        return pick(values, null);
    }
    public static <Entry extends Comparable<? super Entry>> Entry pick(Entry[] values, Comparator<Entry> comparator) {
        Trace.values = values; Trace.comparator = comparator;
        Trace.hit(1); Trace.action.run(); Trace.hit(2);
        if (Trace.invoke) {
            if (comparator == null) {
                Trace.comparison = values[0].compareTo(values[0]);
            } else {
                Trace.comparison = comparator.compare(values[0], values[0]);
            }
        }
        Trace.hit(3);
        return (Entry) Trace.result;
    }
}
