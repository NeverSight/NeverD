package fixture;
public abstract class Parent<E,R,C extends Base<R>> extends Base<R> implements Api<R> {
    protected Parent() {} protected abstract R get(); public <X> X echo(X value) { return value; }
}
