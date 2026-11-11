package fixture;
public interface Context {
    interface Element extends Context { }
    interface Key<E extends Element> { }
}
