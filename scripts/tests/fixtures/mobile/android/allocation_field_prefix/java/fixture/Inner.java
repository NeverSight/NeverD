package fixture;
public final class Inner {
    static { Trace.hit(3); }
    public final Input input;
    public Inner(Input input) {
        Trace.inners++;
        Trace.hit(4);
        this.input = input;
    }
}
