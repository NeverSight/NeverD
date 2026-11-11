package fixture;
public final class NominalOwner implements NominalApi {
    public NominalLeaf selected;
    public RuntimeException failure;
    public int calls;
    public <R, S> R echo(R input, S tag) { return input; }
    public NominalLeaf get() {
        calls++;
        if (failure != null) throw failure;
        return selected;
    }
}
