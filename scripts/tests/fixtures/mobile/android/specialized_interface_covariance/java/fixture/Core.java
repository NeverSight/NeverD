package fixture;

public class Core implements Contract<Number> {
    public Byte value;
    public int calls;
    public Object seen;
    public Number observed;
    public RuntimeException failure;
    public Core(Byte value) { this.value = value; }
    public Byte get(Carrier<Number, String> carrier) {
        calls++;
        seen = carrier;
        if (failure != null) throw failure;
        if (carrier != null) {
            observed = carrier.first;
            carrier.second = "visited";
        }
        return value;
    }
}
