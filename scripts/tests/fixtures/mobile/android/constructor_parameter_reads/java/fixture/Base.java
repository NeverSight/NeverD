package fixture;

public class Base {
    public Base(int value) {
        Trace.base = this;
        Trace.baseValue = value;
        Trace.hit(4);
    }
}
