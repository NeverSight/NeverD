package fixture;
import java.util.*;
public class Factory<E> {
    public final List<Comparator<E>> values;
    public Factory(List<Comparator<E>> values) { Trace.created++;this.values=values; }
    @SuppressWarnings("unchecked")
    public static <E> Factory<E> from(List<Comparator<E>> source) {
        Trace.hit(2);Trace.input=source;Trace.action.run();Trace.hit(3);return (Factory<E>)Trace.result;
    }
}
