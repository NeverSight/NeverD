package fixture;

public class Base<R> implements Arity<R>, java.io.Serializable {
    private final int arity;
    public Base(int arity) { this.arity = arity; }
    public int getArity() { return arity; }
}
