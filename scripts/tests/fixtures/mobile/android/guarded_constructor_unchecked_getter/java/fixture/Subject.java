package fixture;

public final class Subject extends Base {
    public Subject(Class type) {
        this(Provider.make(type, new Object[0]));
    }
    public Subject(Class type, Object[] arguments) {
        this(Provider.make(type, arguments));
        Trace.subjects++;
        Trace.stage = 6;
    }
    public Subject(Object value) {
        super(value);
        Trace.objects++;
        Trace.stage = 5;
    }
}
