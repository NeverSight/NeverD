package fixture;

public class Wide extends Base {
    public Wide(Trace trace, Object value) throws Exception, IllegalArgumentException {
        super(trace, value);
        trace.hit(4, this, value);
    }
}
