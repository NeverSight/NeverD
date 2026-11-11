package fixture;
public final class Core {
    public final Object value;
    public Core(Object[] buffer, int size, int index, Object seed, Factory factory) {
        this(factory.make(buffer = new String[size], buffer[index] = seed));
    }
    private Core(Object value) {
        Trace.hit(2, this);
        this.value = value;
        Trace.core = this;
        Trace.hit(3, this);
    }
}
