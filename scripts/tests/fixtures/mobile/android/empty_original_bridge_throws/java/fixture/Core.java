package fixture;
public final class Core extends Parent<Box> {
    public int calls;
    public Box seen;
    public Box visit(Box value, int[] marker) throws First, Second {
        calls++;
        seen = value;
        Box answer = super.observe(value, marker);
        marker[0] += 11;
        return answer;
    }
}
