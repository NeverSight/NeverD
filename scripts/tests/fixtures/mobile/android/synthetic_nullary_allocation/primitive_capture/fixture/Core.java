package fixture;
import java.util.Map;
import java.util.Collection;
public class Core<K,V> implements Tagged {
    public final Map<? extends K,? extends Collection<V>> original;
    public final Maker<? extends Collection<V>> factory;
    public final Collection<V> created;
    public final float factor;
    @SuppressWarnings("unchecked")
    public Core(boolean flag,float factor,Map<? extends K,? extends Collection<V>> source){this(factor,source,(Maker<? extends Collection<V>>)(Object)new Adapter(flag));Trace.hit(5);}
    @SuppressWarnings("unchecked")
    public Core(Map<? extends K,? extends Collection<V>> source,float factor,boolean flag){this(factor,source,(Maker<? extends Collection<V>>)(Object)new Adapter(flag));Trace.hit(5);}
    @SuppressWarnings("unchecked")
    public Core(float factor,Map<? extends K,? extends Collection<V>> source,Maker<? extends Collection<V>> maker){
        Trace.targets++;Trace.map=source;Trace.factor=factor;Trace.factory=maker;Trace.hit(2);
        Collection<V> value=(Collection<V>)maker.make();Trace.created=value;Trace.hit(3);
        this.original=source;this.factory=maker;this.created=value;this.factor=factor;Trace.instance=this;
        Trace.targetAction.run();Trace.hit(4);
    }
    public Core(float factor,Map<? extends K,? extends Collection<V>> source,Object other){this.original=source;this.factory=null;this.created=null;this.factor=factor;Trace.wrong++;throw new AssertionError("wrong owned target");}
}
