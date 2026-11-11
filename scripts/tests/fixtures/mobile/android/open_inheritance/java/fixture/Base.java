package fixture;

public abstract class Base<P> {
    public Base() {}
    public abstract void apply(P value, Option option);
    public final void apply(P value) {
        apply(value, null);
    }
    public abstract Cell<P, ?> result();
}
