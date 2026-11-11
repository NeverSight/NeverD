package fixture;
import java.util.Map;
import java.util.SortedMap;
import java.util.HashMap;
public class Fresh<A,B> extends HashMap<A,B> {
    public Fresh(Map<? extends A,? extends B> value){super(value);Trace.defaults++;Trace.created=this;Trace.source=value;Trace.hit(1);Trace.allocateAction.run();}
    public Fresh(SortedMap<? extends A,? extends B> value){super();Trace.wrong++;throw new AssertionError("wrong owned sorted initializer");}
}
