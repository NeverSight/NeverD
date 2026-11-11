package fixture;
import java.util.*;
@SuppressWarnings("rawtypes")
public final class Proxy implements Comparator {
    public final String[] names;
    public final Class type;
    public final Object tag=new Object();
    public Proxy(String[] names,Class type) {
        Trace.created++;Trace.hit(1);this.names=names;this.type=type;Trace.proxy=this;
        Trace.proxyAction.run();Trace.hit(2);
    }
    public int compare(Object left,Object right) {
        Trace.hit(5);Trace.left=left;Trace.right=right;Trace.compareAction.run();return (Integer)Trace.result;
    }
}
