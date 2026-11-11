package fixture;
public final class Carrier {
    public int calls;
    public int value;
    public RuntimeException failure;
    public Carrier(int value) { this.value = value; }
    public int readBits() { calls++; if (failure != null) throw failure; return value; }
}
