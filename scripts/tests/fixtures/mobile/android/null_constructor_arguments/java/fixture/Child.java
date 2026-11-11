package fixture;
public final class Child extends Owner {
    static { Trace.step(5); }
    public Child(int value) {
        super(value, null);
        Trace.step(6);
    }
}
