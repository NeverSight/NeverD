package fixture;
public abstract class Parent<X, Y> extends Root<X, Y> {
    public int parentCalls;
    public String parentMarker;
    public int parentIndex;
    public Y parentSeen;
    public long parentWide;
    public boolean parentEnabled;
    public RuntimeException parentFailure;
    protected void read(String marker, int index, Y box, long wide, boolean enabled) {
        parentCalls++; parentMarker = marker; parentIndex = index; parentSeen = box;
        parentWide = wide; parentEnabled = enabled;
        if (parentFailure != null) throw parentFailure;
    }
}
