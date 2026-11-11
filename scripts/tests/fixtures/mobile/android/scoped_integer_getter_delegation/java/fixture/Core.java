package fixture;
import java.util.Map;
import java.util.Collection;
public class Core<K,V> implements Tagged {
    public int size;
    public float factor;
    public Maker<Collection<V>> factory;
    public Collection<V> created;
    public Core(float factor,Map<? extends K,? extends Collection<V>> source,Maker<Collection<V>> maker){this(source.size(),factor,maker);Trace.afterSource=source;Trace.hit(4);}
    public Core(Map<? extends K,? extends Collection<V>> source,float factor,Maker<Collection<V>> maker){this(source.size(),factor,maker);Trace.afterSource=source;Trace.hit(4);}
    @SuppressWarnings("unchecked")
    public Core(int size,float factor,Maker<Collection<V>> maker){
        Trace.targets++;Trace.snapshot=size;Trace.factor=factor;Trace.factory=maker;Trace.hit(2);
        Collection<V> value=(Collection<V>)maker.make();Trace.created=value;Trace.hit(3);
        this.size=size;this.factor=factor;this.factory=maker;this.created=value;Trace.instance=this;
        Trace.targetAction.run();
    }
    public Core(int size,float factor,Object other){Trace.wrong++;throw new AssertionError("wrong owned target");}
}
