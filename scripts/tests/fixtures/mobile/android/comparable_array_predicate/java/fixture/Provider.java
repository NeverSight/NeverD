package fixture;
public class Provider {
    public static <Entry extends Comparable<? super Entry>> boolean test(Entry[] values) {
        Trace.values = values;
        Trace.hit(1); Trace.action.run(); Trace.hit(2);
        if (Trace.read) Trace.comparison = values[0].compareTo(values[0]);
        Trace.hit(3);
        return ((Boolean) Trace.result).booleanValue();
    }
}
