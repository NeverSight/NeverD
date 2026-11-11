package fixture;
import java.util.Map;
import java.util.function.Function;
public class Parent<K,V> {
    public Map<K,V> value;
    public Function<Object,K> function;
    public K key;
    public Parent(Map<K,V> value,Function<Object,K> function){Trace.parents++;this.value=value;this.function=function;Trace.parent=this;Trace.selected=value;Trace.hit(3);Trace.parentAction.run();this.key=function.apply(Trace.input);Trace.hit(5);}
    public Parent(Map<K,V> value,int marker){Trace.wrong++;throw new AssertionError("wrong owned overload");}
}
