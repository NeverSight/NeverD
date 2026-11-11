package fixture;

public interface ObserverRoot {
    void event(int value);
    default int adjust(int value) { Trace.mark(7); return value + 7; }
}
