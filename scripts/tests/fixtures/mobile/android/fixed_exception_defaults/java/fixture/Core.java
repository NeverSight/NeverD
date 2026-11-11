package fixture;
public class Core implements Api<Core> {
    public int calls;
    public Object seenMarker;
    public String seenName, seenLabel;
    public Carrier<Number, String> seenInput;
    public Number seenValue;
    public Core result;
    public First failure;
    public Second second;
    @Override
    public Core apply(Object marker, String name, Carrier<Number, String> input) throws First, Second {
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
