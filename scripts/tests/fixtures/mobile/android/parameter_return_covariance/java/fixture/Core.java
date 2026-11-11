package fixture;
public class Core extends Root<String, Object> {
    public int calls;
    public String seenKey;
    public Object seenValue;
    public Core result;
    public First failure;
    public Second second;
    @Override
    public Core put(String key, Object value) throws First, Second {
        calls++;
        seenKey = key;
        seenValue = value;
        if (failure != null) throw failure;
        if (second != null) throw second;
        return result;
    }
}
