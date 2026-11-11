package fixture;
public class Worker extends Base {
    @Override public Base update(int code, Object[] values, long wide, double real) {
        Trace.hit(1);
        Base result = super.update(code, values, wide, real);
        Trace.hit(4);
        return result;
    }
}
