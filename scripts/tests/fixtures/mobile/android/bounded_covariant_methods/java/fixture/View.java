package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public class View<T> implements Contract<T> {
    public Object last;
    public ResultLeaf stored;
    public View() {}
    public <R extends Bound> ResultLeaf<R> make(Pair<T, R> input) {
        Trace.events = Trace.events * 10 + 1;
        last = input;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return (ResultLeaf<R>) stored;
    }
}
