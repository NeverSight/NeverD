package fixture;
import java.util.Map;
public class Core<K,V> extends Parent<K,V> implements Tagged<K> {
    public Core(Map<? extends K,? extends V> value){this(0.625f,value);Trace.wrappers++;Trace.hit(6);}
    public Core(Map<? extends K,? extends V> value,int marker){this(-0.0d,value);Trace.wrappers++;Trace.hit(6);}
    public Core(float scalar,Map<? extends K,? extends V> value){super(value);Trace.children++;Trace.scalar32=scalar;Trace.child=this;Trace.hit(4);Trace.childAction.run();Trace.hit(5);}
    public Core(double scalar,Map<? extends K,? extends V> value){super(value);Trace.children++;Trace.scalar64=scalar;Trace.child=this;Trace.hit(4);Trace.childAction.run();Trace.hit(5);}
    public Core(int scalar,Map<? extends K,? extends V> value){super(value);Trace.wrong++;throw new AssertionError("wrong owned int overload");}
    public Core(long scalar,Map<? extends K,? extends V> value){super(value);Trace.wrong++;throw new AssertionError("wrong owned long overload");}
}
