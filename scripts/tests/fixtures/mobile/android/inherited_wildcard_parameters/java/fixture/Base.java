package fixture;
public class Base<T> {
    public int baseCalls;
    public Object baseInput;
    public RuntimeException baseFailure;
    public boolean remove(Object entry) { baseCalls++; baseInput = entry; if (baseFailure != null) throw baseFailure; return entry != null; }
}
