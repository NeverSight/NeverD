package fixture;
public interface Root<E> extends java.util.List<E> {
    @SuppressWarnings("unchecked")
    @Override default View<E> subList(int from, int to) {
        Trace.seen = this;
        Trace.first = from;
        Trace.second = to;
        Trace.hit(3);
        View<E> output = (View<E>)Trace.result;
        Trace.hit(4);
        return output;
    }
}
