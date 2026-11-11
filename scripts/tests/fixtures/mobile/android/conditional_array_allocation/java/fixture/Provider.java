package fixture;
public final class Provider {
    public int first;
    public int second;
    public int reads;
    public int arrayReads;
    public Object[] data;
    public Object fallback;
    public int index() {
        reads++;
        Trace.event(reads == 1 ? 2 : 4);
        return reads == 1 ? first : second;
    }
    public Object[] values() {
        arrayReads++;
        Trace.event(3);
        return data;
    }
}
