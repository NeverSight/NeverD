package fixture;
public class Base<T> implements Tag<Object> {
    static { Trace.step(1); }
    public Object context;
    public Object[] values;
    public int count, index;
    public long bits;
    public Base(Object context, int count, int index, T[] input, long bits) {
        Trace.step(2);
        this.context = context; this.count = count; this.index = index;
        this.values = input; this.bits = bits;
        Trace.observe(this);
        Trace.step(8);
        ((Object[]) input)[index] = Trace.replacement;
        Trace.step(3);
    }
    public Base(Object context, int count, int index, java.util.List<T> input, long bits) {
        Trace.step(9);
    }
}
