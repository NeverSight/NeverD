package fixture;
import java.util.function.Function;
public class Target<K,V> {
    public final Box<K,V> box;
    public final Function<V,K> mapper;
    public Target(Function<V,K> mapper) { this(false,mapper); }
    public Target(boolean flag,Function<V,K> mapper) { this(Box.<K,V>create(flag),mapper); }
    public Target(int capacity,boolean flag,Function<V,K> mapper) { this(Box.<K,V>create(capacity,flag),mapper); }
    public Target(Box<K,V> box,Function<V,K> mapper) {
        Trace.hit(3);Trace.partial=this;Trace.box=box;this.box=box;
        Trace.hit(4);this.mapper=mapper;Trace.hit(5);
    }
}
