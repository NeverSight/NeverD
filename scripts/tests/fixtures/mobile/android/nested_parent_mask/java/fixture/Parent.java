package fixture;
public abstract class Parent<N extends Bound> implements View {
    public final int boot;
    protected Parent() { Trace.parent=this;Trace.hit(4);boot=17;Trace.hit(5); }
}
