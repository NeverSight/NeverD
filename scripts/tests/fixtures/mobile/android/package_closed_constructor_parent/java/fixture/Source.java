package fixture;

public class Source {
    public int first;
    public int second;
    public int first() {
        Trace.hit(0, null, this, first);
        return first;
    }
    public int second() {
        Trace.hit(1, null, this, second);
        return second;
    }
}
