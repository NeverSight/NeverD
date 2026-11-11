package fixture;

public abstract class Base<P> {
    public Base() {}
    public abstract P echo(P value, Option option);
    public final P echo(P value) { return echo(value, null); }
    public abstract Box<P, ?> box();
}
