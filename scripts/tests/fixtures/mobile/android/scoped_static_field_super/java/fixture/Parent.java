package fixture;
import java.lang.ref.Reference;
import java.util.concurrent.ConcurrentMap;
public class Parent<K,V> {
    public final ConcurrentMap<Reference<K>,V> original;
    public final Marker marker;
    public Parent(ConcurrentMap<Reference<K>,V> value,Marker marker){
        Trace.parentCalls++;Trace.received=value;Trace.observed=marker;Trace.hit(1);
        Trace.action.run();Trace.hit(2);
        this.original=value;this.marker=marker;Trace.parentObject=this;Trace.hit(3);
    }
}
