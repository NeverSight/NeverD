package fixture;
import java.util.Map;
public class Core extends Parent<String,Object> implements Tagged<String> {
    public Core(Map<String,Object> source){super(source==null?new DefaultMap<String,Object>():source);Trace.hit(4);}
    public Core(Map<String,Object> source,int kind){super(source!=null?new DefaultMap<String,Object>():source);Trace.hit(4);}
}
