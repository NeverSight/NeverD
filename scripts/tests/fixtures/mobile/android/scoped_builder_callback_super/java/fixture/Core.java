package fixture;
import java.util.function.Function;
import java.io.Serializable;
public class Core<K,V> extends Parent<K,V> implements Tagged<K> {
    @SuppressWarnings("unchecked") public Core(Builder<K,V> builder){super(builder.build(),(Function<Object,K>&Serializable)(Object)new Adapter());Trace.children++;Trace.hit(6);}
    @SuppressWarnings("unchecked") public Core(Builder<K,V> builder,int marker){super(builder.build(),(Function<Object,K>)(Object)(Serializable)(Object)new Adapter());Trace.children++;Trace.hit(6);}
}
