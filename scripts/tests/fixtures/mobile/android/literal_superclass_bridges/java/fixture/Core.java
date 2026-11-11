package fixture;
public class Core extends Middle {
    static { Trace.seed = 100; Trace.hit(9); }
    public void accept(Narrow value, long stamp, Object note) {
        Trace.hit(1);
        Trace.consume(note);
        Trace.last = value;
        Trace.number = stamp;
        Trace.hit(2);
    }
}
