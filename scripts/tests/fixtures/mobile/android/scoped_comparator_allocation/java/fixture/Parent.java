package fixture;
import java.util.Map;
public class Parent<K,V> {
    public Map<K,V> value;
    @SuppressWarnings("unchecked")
    public Parent(Map<? extends K,? extends V> value){Trace.parents++;Trace.hit(2);Trace.selected=value;this.value=(Map<K,V>)value;Trace.parent=this;Trace.parentAction.run();Trace.hit(3);}
    public Parent(int value){Trace.wrong++;throw new AssertionError("wrong owned overload");}
    @SuppressWarnings("unchecked")
    public K key(){return (K)Trace.key;}
}
