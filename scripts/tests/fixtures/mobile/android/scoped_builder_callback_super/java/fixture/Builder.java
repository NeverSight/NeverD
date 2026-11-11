package fixture;
import java.util.Map;
public class Builder<K,V> implements Maker<Map<K,V>> {
    public Map<K,V> value;
    public Builder(Map<K,V> value){this.value=value;}
    @SuppressWarnings("unchecked") public Map<K,V> build(){Trace.builds++;Trace.builder=this;Trace.hit(1);Map<K,V> result=(Map<K,V>)Trace.mapResult;Trace.buildAction.run();return result;}
}
