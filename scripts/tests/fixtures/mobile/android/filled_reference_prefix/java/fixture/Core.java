package fixture;
public class Core extends Base {
    public Core(Object first,Object second) { this(new Object[]{first,second}); }
    public Core(Object[] first,Object[] second) { this(new Object[][]{first,second}); }
    public Core(Object first,Object second,int marker) {
        super(Factory.prepare(new Object[]{first,second}));
        Trace.hit(7); Trace.coreSelf=this; Trace.after.run(); Trace.hit(8);
    }
    public Core(Object[] first,Object[] second,int marker) {
        super(Factory.prepare(new Object[][]{first,second}));
        Trace.hit(7); Trace.coreSelf=this; Trace.after.run(); Trace.hit(8);
    }
    public Core(Object[] values) {
        super(Factory.prepare(values));
        Trace.hit(7); Trace.coreSelf=this; Trace.after.run(); Trace.hit(8);
    }
    public Core(Object[][] values) {
        super(Factory.prepare(values));
        Trace.hit(7); Trace.coreSelf=this; Trace.after.run(); Trace.hit(8);
    }
}
