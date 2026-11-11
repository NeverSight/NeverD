package fixture.dependency;

public final class Covariant<C extends Root<? extends Tag>> {
    public final C node;
    public int accesses;
    public Covariant(C node) { this.node = node; }
    public C node() { ++accesses; return node; }
}
