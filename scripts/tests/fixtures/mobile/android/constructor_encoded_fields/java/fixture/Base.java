package fixture;
public class Base {
    public int word;
    public long wide;
    public Object payload;
    public Base(Object payload, int word, long wide) {
        Trace.baseCalls++;
        this.word = word; this.wide = wide; this.payload = payload;
        Holder.word = Trace.nextWord; Holder.wide = Trace.nextWide;
        Trace.hit(2);
        if (payload == null) throw Trace.failure;
        Trace.hit(3);
    }
}
