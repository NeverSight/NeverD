package fixture;
public class Core<K,V,C extends java.util.Collection<V>> extends Parent<K,C> implements Tagged<K> {
    public Core(int capacity,float factor){super(new Fresh<K,C>(capacity,factor));Trace.hit(4);}
    public Core(float factor,int capacity){super(new Fresh<K,C>(capacity,factor));Trace.hit(4);}
}
