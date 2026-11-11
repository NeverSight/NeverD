package fixture;

public final class Direct extends Base {
    public Direct(Class<?> type) {
        super(Provider.make(type, new Object[0]));
        Trace.directs++;
        Trace.stage = 5;
    }
    public Direct(Class<?> type, Object[] arguments) {
        super(Provider.make(type, arguments));
        Trace.directs++;
        Trace.stage = 5;
    }
}
