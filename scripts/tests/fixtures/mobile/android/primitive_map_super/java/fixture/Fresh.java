package fixture;
import java.util.LinkedHashMap;
public class Fresh<A,B> extends LinkedHashMap<A,B> {
    public Fresh(int capacity,float factor){super(capacity,factor);Trace.defaults++;Trace.hit(1);Trace.created=this;Trace.capacity=capacity;Trace.factor=factor;Trace.allocateAction.run();}
    public Fresh(float factor,int capacity){super();Trace.wrong++;throw new AssertionError("wrong owned allocation overload");}
}
