package fixture;
import java.util.Comparator;
public class Provider {
    @SuppressWarnings("unchecked")
    public static <T extends Comparable<? super T>> Comparator<T> select(){Trace.calls++;Trace.hit(1);Trace.action.run();Trace.hit(2);return (Comparator<T>)Trace.result;}
}
