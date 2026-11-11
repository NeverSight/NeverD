package fixture;
public final class Outer {
    static { Trace.step(1); }
    public final Inner value;
    public Outer(Inner value) {
        this.value = value; Trace.outer = this;
        Trace.step(6);
    }
}
