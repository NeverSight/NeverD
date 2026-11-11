package fixture;
import java.util.Map;
public class Parent<K,V> {
    public Map<K,V> map;
    public Parent(Map<K,V> value){this.map=value;Trace.parent=this;Trace.parents++;Trace.hit(2);Trace.parentAction.run();}
    public Parent(int value){Trace.wrong++;throw new AssertionError("wrong owned parent");}
}
