package fixture;
import java.util.HashMap;
public class DefaultMap<K,V> extends HashMap<K,V> {
    public DefaultMap(){Trace.defaults++;Trace.hit(1);Trace.created=this;Trace.allocateAction.run();}
}
