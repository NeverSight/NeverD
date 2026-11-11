package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public class View<T> implements Contract<T> {
    public Object last;
    public Pair stored;
    public View() {}
    public <R extends Bound<? super R>> Pair<R, R> make(Pair<T, R> input) {
        Trace.events = Trace.events * 10 + 1;
        last = input;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return (Pair<R, R>) stored;
    }
}
