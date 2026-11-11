import fixture.*;
public class PlainInterfaceCovarianceDerived extends Worker {
    @Override public Api update(int code, Object[] values, long wide, double real) {
        Trace.subclassCalls++;
        return super.update(code, values, wide, real);
    }
    @Override public Api update(Object value) {
        Trace.subclassCalls++;
        return super.update(value);
    }
}
