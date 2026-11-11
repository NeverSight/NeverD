package fixture;
public class Reader extends Base {
    public Reader(Object payload) { this(payload, Holder.word, Holder.wide); }
    public Reader(Object payload, int word, long wide) {
        super(payload, word, wide);
        Trace.hit(4);
    }
}
