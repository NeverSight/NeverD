package fixture;
public class NominalLeaf implements NominalBase {
    public int id;
    public NominalLeaf(int id) { this.id = id; }
    public int tag() { return id; }
}
