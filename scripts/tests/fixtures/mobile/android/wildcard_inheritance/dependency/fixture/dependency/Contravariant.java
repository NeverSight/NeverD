package fixture.dependency;

public final class Contravariant<C extends Root<? super ChildTag>> {
    public final C node;
    public int accesses;
    public Contravariant(C node) { this.node = node; }
    public C node() { ++accesses; return node; }
}
