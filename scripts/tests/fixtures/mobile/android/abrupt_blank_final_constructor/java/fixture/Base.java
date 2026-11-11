package fixture;

public class Base {
    public Base() {
        Trace.owner = this;
        Trace.hit(1);
    }
}
