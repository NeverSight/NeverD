package fixture;

public class Base {
    public final int seen;

    public Base(int input) {
        super();
        seen = value() + input;
    }

    public int value() { return -1; }
    public Object token() { return null; }
    public int extra() { return 0; }
}
