package fixture;
public class Parent {
    public final long bits;
    public final Object token;
    public Parent(long bits,Object token) {
        this.bits=bits;this.token=token;
        Trace.partial=(Child)this;
        Trace.hit(3);
        Trace.observer.prepared((Child)this);
        Trace.hit(4);
    }
}
