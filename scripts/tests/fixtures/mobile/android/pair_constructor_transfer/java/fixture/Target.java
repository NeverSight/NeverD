package fixture;
import java.util.HashMap;
import java.util.function.Function;
public class Target<K,V> {
    public final Box<K,V> box;
    public final Function<V,K> mapper;
    public Target(int capacity,float load,Function<V,K> mapper) {
        this(Box.<K,V>wrap(new HashMap<K,V>(capacity,load)),mapper);
    }
    public Target(Box<K,V> box,Function<V,K> mapper) {
        Trace.hit(3);Trace.partial=this;Trace.box=box;this.box=box;
        Trace.hit(4);this.mapper=mapper;Trace.hit(5);
    }
}
