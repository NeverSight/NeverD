package fixture;
public final class Value extends Bound implements Flag {
    public Value(int tag) { super(tag); }
    public void touch() { Trace.hit(7); Trace.seenTag = tag; }
}
