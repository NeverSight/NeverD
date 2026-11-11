package fixture;

public abstract class DefaultParent<X,Y> implements Leaf<X>, Root<X> {
    public DefaultParent() {}
    public abstract X value(X next);
}
