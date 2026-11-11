package fixture;
public final class Item implements Bound {
    public final long bits;
    public Item(long bits) { this.bits=bits; }
    public long value() { return bits; }
}
