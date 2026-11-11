package fixture;
import java.util.*;
public class Target<E> {
    public final List<Comparator<E>> values;
    public final Context context;
    public Target() { this(new Bucket<Comparator<E>>(),new Context()); }
    public Target(List<Comparator<E>> values,Context context) {
        Trace.hit(3);this.values=values;this.context=context;Trace.target=this;
        Trace.action.run();Trace.hit(4);
    }
}
