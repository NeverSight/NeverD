package fixture;
public class Core extends Base {
    public Core(Object head,Object first,Object second) {
        this(head,new Wrap(new Leaf(first),second));
    }
    public Core(Object head,Value value) {
        super(head,value);
        Trace.hit(11); Trace.coreSelf=this; Trace.after.run(); Trace.hit(12);
    }
    public Core(Object head,Object first,Object second,int marker) {
        super(head,new Wrap(new Leaf(first),second));
        Trace.hit(11); Trace.coreSelf=this; Trace.after.run(); Trace.hit(12);
    }
}
