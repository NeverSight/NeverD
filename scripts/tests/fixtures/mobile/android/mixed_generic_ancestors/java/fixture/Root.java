package fixture;

public class Root {
    public int seed;
    public Root(int seed) { this.seed = seed; }
    public int count() { return seed; }
}
