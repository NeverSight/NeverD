package fixture;
public class Worker implements Api {
    public Api replacement;
    public Class<?> witness;
    public int calls;
    public Object seen;
    public Object[] array;
    public int code;
    public long wide;
    public double real;
    public Api update(int code, Object[] values, long wide, double real) {
        Trace.hit(1);
        calls++;
        this.code = code;
        this.array = values;
        this.wide = wide;
        this.real = real;
        seen = values[0];
        if (Trace.failure != null) throw Trace.failure;
        Trace.hit(2);
        return replacement;
    }
    public Api update(Object value) {
        Trace.hit(3);
        calls++;
        seen = value;
        if (Trace.failure != null) throw Trace.failure;
        Trace.hit(4);
        return replacement;
    }
    public int scalar() { return calls; }
}
