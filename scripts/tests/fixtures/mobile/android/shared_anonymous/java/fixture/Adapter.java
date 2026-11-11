package fixture;

public class Adapter {
    private final SharedHost.Worker.Child child;
    Adapter(SharedHost.Worker.Child child) { this.child = child; }
    public int value() { return child.value(); }
}
