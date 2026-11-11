package fixture;
public final class Carrier {
    public int calls;
    public long value;
    public RuntimeException failure;
    public Carrier(long value) { this.value = value; }
    public long readBits() { calls++; if (failure != null) throw failure; return value; }
}
