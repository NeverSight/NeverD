package fixture;
public class Core<K,V> extends Parent<K,V> implements Tagged<K> {
    public Core(int capacity,float factor){super(new Fresh<K,V>(capacity,factor));Trace.hit(4);}
    public Core(float factor,int capacity){super(new Fresh<K,V>(capacity,factor));Trace.hit(4);}
}
