package fixture;
public class Data<P, Q> implements Carrier<P, Q> {
    public Object value;
    public Data(Object value) { this.value = value; }
    public Object tag() { return value; }
}
