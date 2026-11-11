package fixture;
public class Base {
    public final Object head;
    public final Value value;
    public Base(Object head,Value value) {
        Trace.hit(9); Trace.baseSelf=this;
        this.head=head; this.value=value;
        Trace.baseAction.run(); Trace.hit(10);
    }
}
