package fixture;
public class Leaf implements Base {
    public Object tag;
    public Leaf(Object tag) { this.tag = tag; }
    public Object marker() { return tag; }
}
