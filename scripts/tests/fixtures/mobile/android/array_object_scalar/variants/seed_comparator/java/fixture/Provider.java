package fixture;
public class Provider {
    public static <E> int score(E[] values,E key,java.util.Comparator<? super E> comparator) {
        Trace.calls++;Trace.values=values;Trace.key=key;Trace.seenComparator=comparator;Trace.hit(1);
        Trace.action.run();Trace.hit(2);return ((Integer)Trace.result).intValue();
    }
}
