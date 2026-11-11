import fixture.*;
public class PlainInterfaceCovarianceViaDefault implements Api {
    private final Worker worker;
    public PlainInterfaceCovarianceViaDefault(Worker worker) { this.worker=worker; }
    @Override public Api update(int code, Object[] values, long wide, double real) {
        Trace.defaultCalls++;
        return worker.update(code,values,wide,real);
    }
    @Override public Api update(Object value) {
        Trace.defaultCalls++;
        return worker.update(value);
    }
    @Override public int scalar() { return worker.scalar(); }
}
