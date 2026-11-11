package fixture;
public final class Carrier<Q> {
    public Q value;
    public long bits;
    public Carrier(Q value, long bits) { this.value = value; this.bits = bits; }
}
