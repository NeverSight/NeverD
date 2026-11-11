package fixture;
public final class Core {
    public final Object value;
    public Core(Object prior, Factory factory, int size, int mask) {
        this((mask & 1) != 0 ? factory.make(new Object[size]) : prior);
    }
    private Core(Object value) {
        Trace.hit(2, this);
        this.value = value;
        Trace.core = this;
        Trace.hit(3, this);
    }
}
