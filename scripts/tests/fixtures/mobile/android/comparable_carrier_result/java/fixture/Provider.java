package fixture;
import java.util.Collection;
public class Provider {
    public static <Entry extends Comparable<? super Entry>> Entry select(Collection<Entry> values) {
        Trace.input=values;Trace.hit(1);
        Trace.action.run();Trace.hit(2);
        return (Entry)Trace.result;
    }
}
