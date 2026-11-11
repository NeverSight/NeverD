package fixture.dependency;

public final class Unbounded<C extends Root<?>> {
    public final C node;
    public int accesses;
    public Unbounded(C node) { this.node = node; }
    public C node() { ++accesses; return node; }
}
