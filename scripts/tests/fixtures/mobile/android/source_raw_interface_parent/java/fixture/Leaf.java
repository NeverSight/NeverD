package fixture;
public interface Leaf extends Root {
    default <R> R fold(R value) {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.secondFailure != null) throw Trace.secondFailure;
        return value;
    }
}
