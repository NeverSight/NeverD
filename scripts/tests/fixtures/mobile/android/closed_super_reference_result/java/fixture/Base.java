package fixture;
import java.io.IOException;
public class Base<K,V> {
    public V visit(K value,V items,IOException failure)throws IOException {
        Trace.hit(2);Trace.value=value;Trace.items=items;Trace.error=failure;
        Trace.action.run();
        if(failure!=null)throw failure;
        Trace.hit(3);return (V)Trace.result;
    }
}
