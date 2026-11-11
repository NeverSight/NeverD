package fixture;

public class Child<X extends Enum<X>, Y extends Enum<Y>> extends Parent<X> {
    public X chosen;
    public int calls;
    public RuntimeException failure;
    public Child() { super(); }
    public X parse(String text) {
        calls++;
        if (failure != null) throw failure;
        return chosen;
    }
}
