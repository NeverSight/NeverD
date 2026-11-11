package fixture;
public final class Target<T> {
    static {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
    }
    public T value;
    public Object original;
    public long stamp;
    public double fraction;
    public Target(T item, Object input, long word, double bits) {
        Trace.events = Trace.events * 10 + 4; Trace.constructors++;
        if (Trace.constructorFailure != null) throw Trace.constructorFailure;
        value = item; original = input; stamp = word; fraction = bits;
    }
}
