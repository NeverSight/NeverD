package fixture;
public class Parent<E> {
    public final int boot;
    protected Parent() { Trace.parent=this;Trace.hit(7);boot=2; }
}
