package fixture;
public final class Target {
    public Object first, second, third, fourth;
    public boolean option, captured;
    static { Trace.hit(1, null); }
    public Target(Object first, Object second, Object third, Object fourth, boolean option, boolean captured) {
        this.first = first; this.second = second; this.third = third; this.fourth = fourth;
        this.option = option; this.captured = captured;
        Trace.hit(2, this);
    }
}
