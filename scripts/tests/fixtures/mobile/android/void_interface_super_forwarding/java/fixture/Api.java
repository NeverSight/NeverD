package fixture;
public interface Api extends Shared {
    default void signal() {
        Trace.begin(this);
        base();
        Trace.after(this);
    }
}
