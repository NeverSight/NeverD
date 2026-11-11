package fixture;
public class Bound {
    public boolean accepted;
    public int reads;
    public RuntimeException failure;
    public boolean read() { reads++; if (failure != null) throw failure; return accepted; }
}
