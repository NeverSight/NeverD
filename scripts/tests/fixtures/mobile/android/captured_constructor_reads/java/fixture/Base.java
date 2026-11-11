package fixture;
public abstract class Base {
    public Object payload, spare;
    public int word;
    public long wide;
    public Base(Object payload, int word, Object spare) {
        this.payload = payload; this.spare = spare; this.word = word;
        Trace.baseCalls++;
        Trace.observed = this;
        Trace.captured = owner();
        Trace.later = Later.value;
        Trace.hit(3);
        if (payload == null) throw Trace.failure;
    }
    public Base(Object payload, long wide, Object spare) {
        this.payload = payload; this.spare = spare; this.wide = wide;
        Trace.baseCalls++;
        Trace.observed = this;
        Trace.captured = owner();
        Trace.later = Later.value;
        Trace.hit(3);
        if (payload == null) throw Trace.failure;
    }
    public abstract Owner owner();
}
