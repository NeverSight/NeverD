package fixture;
public class Receiver extends Producer<String> implements PlainApi {
    public SpecificValue value;
    public int calls;
    public RuntimeException failure;
    public Receiver() {}
    @Override public SpecificValue create() {
        calls++;
        if (failure != null) throw failure;
        return value;
    }
}
