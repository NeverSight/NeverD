package fixture;
public class Base {
    public final Object[] values;
    public Base(Object[] values) {
        Trace.hit(5);
        this.values=values;
        Trace.baseSelf=this;
        Trace.baseAction.run();
        Trace.hit(6);
    }
}
