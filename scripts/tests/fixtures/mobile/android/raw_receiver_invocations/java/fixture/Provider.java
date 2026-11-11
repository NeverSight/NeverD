package fixture;
public class Provider<T> implements Api<T> {
    public <U> Box<? extends U> read(Box<U> input, T tag, long bits, double value) {
        Trace.hit(2);
        Trace.receiver = this; Trace.input = input; Trace.tag = tag;
        Trace.bits = bits; Trace.value = value;
        Trace.hit(3);
        return input;
    }
}
