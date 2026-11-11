package fixture;
import java.util.Map;
import java.util.function.Supplier;
import java.util.function.Function;
public class Core<K,V> extends Parent<K,V> implements Tagged<K> {
    public final Function<Object,K> keys;
    public final Function<Object,V> values;
    public Core(Map<K,V> source,Function<Object,K> keys,Function<Object,V> values){super(source);this.keys=keys;this.values=values;Trace.core=this;Trace.constructors++;Trace.hit(3);Trace.constructorAction.run();}
    public Core(Supplier<Map<K,V>> source,Function<Object,K> keys,Function<Object,V> values){this(source.get(),keys,values);Trace.hit(4);}
    public Core(Supplier<Map<K,V>> source,Function<Object,K> keys,Function<Object,V> values,int marker){this(source.get(),keys,values);Trace.hit(4);}
    public K readKey(){return keys.apply(Trace.input);}
    public V readValue(){return values.apply(Trace.input);}
}
