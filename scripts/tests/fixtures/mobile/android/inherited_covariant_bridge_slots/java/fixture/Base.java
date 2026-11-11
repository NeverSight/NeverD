package fixture;
public class Base implements Editable {
    public Base replacement;
    public Object seen;
    public Object[] array;
    public int code;
    public int calls;
    public long wide;
    public double real;
    @Override public Base update(int code, Object[] values, long wide, double real) {
        Trace.hit(2);
        calls++;
        this.code = code;
        this.array = values;
        this.wide = wide;
        this.real = real;
        seen = values[0];
        if (Trace.failure != null) throw Trace.failure;
        Trace.hit(3);
        return replacement;
    }
    @Override public int scalar() { return calls; }
}
