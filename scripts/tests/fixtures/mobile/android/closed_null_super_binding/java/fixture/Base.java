package fixture;
public class Base extends Root {
    static { Trace.step(1); }
    public Class type;
    public Object input, other;
    public int prepared;
    public long bits;
    public double number;
    public Base(String label, Class<? extends Bound> type, Context input, int prepared, long bits, double number, Context other) {
        super(label);
        Trace.prepare(input);
        Trace.step(3);
        this.type = type;
        this.input = input;
        this.prepared = prepared;
        this.bits = bits;
        this.number = number;
        this.other = other;
        Trace.step(4);
    }
}
