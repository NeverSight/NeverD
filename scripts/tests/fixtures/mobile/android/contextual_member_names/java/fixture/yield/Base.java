package fixture.yield;
public class Base {
    public int yield, record, sealed, permits, var;
    public RuntimeException failure;
    public int yield(int value) {
        if (this.failure != null) throw this.failure;
        return value + this.yield;
    }
    public static int record(int value) { return value * 17; }
}
