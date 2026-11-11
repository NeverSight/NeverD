package fixture;
public final class Provider {
    public Object first;
    public Object second;
    public long data;
    public int calls;
    public Object current() {
        calls++;
        Trace.event(calls==1 ? 2 : 3);
        return calls==1 ? first : second;
    }
    public long bits() { Trace.event(5); return data; }
}
