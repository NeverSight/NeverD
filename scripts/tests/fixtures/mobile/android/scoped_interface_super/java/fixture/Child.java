package fixture;
public abstract class Child<E> extends Base<E> implements Facade<E> {
    static { Trace.hit(9); }
    @Override public View<E> subList(int from, int to) {
        Trace.hit(2);
        View<E> output = Facade.super.subList(from, to);
        Trace.after = output;
        Trace.hit(5);
        return output;
    }
}
