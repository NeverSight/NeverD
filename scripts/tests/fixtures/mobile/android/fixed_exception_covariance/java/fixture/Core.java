package fixture;
public class Core extends Parent<String> {
    public int calls;
    public Object marker;
    public String name;
    public Carrier<Number, String> input;
    public Object seenMarker;
    public String seenName, seenLabel;
    public Carrier<Number, String> seenInput;
    public Number seenValue;
    public Core result;
    public First failure;
    public Second second;
    public Core copy() throws First, Second {
        calls++;
        seenMarker = marker;
        seenName = name;
        seenInput = input;
        seenValue = input.value;
        seenLabel = input.label;
        if (failure != null) throw failure;
        if (second != null) throw second;
        return result;
    }
}
