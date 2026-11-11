package fixture;
public class Carrier<A, B> {
    public A value;
    public B label;
    public Carrier(A value, B label) { this.value = value; this.label = label; }
}
