package fixture;
public class Base<E> implements Api<E> {
    public int scalarCalls, scalarState;
    public Object scalarInput;
    public RuntimeException scalarFailure;
    @Override public boolean add(E item) {
        scalarCalls++;
        scalarState = scalarState * 2 + 1;
        scalarInput = item;
        if (item instanceof int[]) ((int[]) item)[0] += 2;
        if (scalarFailure != null) throw scalarFailure;
        return item != null;
    }
}
