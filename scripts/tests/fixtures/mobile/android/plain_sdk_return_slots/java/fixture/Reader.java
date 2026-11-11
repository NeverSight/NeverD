package fixture;
public class Reader extends SdkWidget implements Api {
    public final State value;
    public int sourceCalls;
    public int identityCalls;
    public Reader(State value) { this.value = value; }
    public State sourceState() {
        sourceCalls += 3;
        if (failure != null) throw failure;
        return value;
    }
    public <N> N identity(N marker) {
        identityCalls++;
        if (failure != null) throw failure;
        return marker;
    }
}
