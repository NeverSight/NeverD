package fixture;
import java.util.Comparator;
public class Core<K,V> extends Parent<K,V> implements Tagged<K> {
    public Core(Comparator<? super K> comparator){super(new Fresh<K,V>(comparator));Trace.hit(4);}
    public Core(Comparator<? super K> comparator,int marker){super(new Fresh<K,V>(comparator));Trace.hit(4);}
}
