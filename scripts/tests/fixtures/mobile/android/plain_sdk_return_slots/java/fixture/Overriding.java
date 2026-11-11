package fixture;
public class Overriding extends ForwardReader {
    public Overriding(State value) { super(value); }
    public State sourceState() {
        sourceCalls += 7;
        if (failure != null) throw failure;
        return value;
    }
}
