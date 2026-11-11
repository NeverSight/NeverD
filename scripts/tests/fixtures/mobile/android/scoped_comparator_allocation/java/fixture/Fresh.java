package fixture;
import java.util.Comparator;
import java.util.Map;
import java.util.HashMap;
public class Fresh<A,B> extends HashMap<A,B> {
    public Comparator<? super A> comparator;
    public Fresh(Comparator<? super A> comparator){super();this.comparator=comparator;Trace.defaults++;Trace.created=this;Trace.capturedComparator=comparator;Trace.hit(1);Trace.allocateAction.run();}
    public Fresh(Map<? extends A,? extends B> value){super(value);Trace.wrong++;throw new AssertionError("wrong owned initializer overload");}
}
