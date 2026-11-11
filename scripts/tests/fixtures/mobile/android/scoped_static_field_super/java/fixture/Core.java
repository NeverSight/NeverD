package fixture;
import java.lang.ref.Reference;
import java.util.concurrent.ConcurrentMap;
public class Core<K,V> extends Parent<K,V> implements Tagged {
    public Core(ConcurrentMap<Reference<K>,V> value){super(value,Marker.FIRST);Trace.hit(4);Trace.completed++;}
    public Core(ConcurrentMap<Reference<K>,V> value,int context){super(value,Trace.selected);Trace.hit(4);Trace.completed++;}
}
