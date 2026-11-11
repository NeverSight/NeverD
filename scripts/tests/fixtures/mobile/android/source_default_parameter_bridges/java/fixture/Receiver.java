package fixture;

public class Receiver implements Facade {
    public int value;
    public int calls;
    public int tag;
    public RuntimeException failure;

    public Receiver() {}

    @Override
    public int order() {
        calls++;
        Trace.events = Trace.events * 10 + tag;
        if (failure != null) throw failure;
        return value;
    }
}
