package fixture;

public class Box<A, B> {
    public A value;
    public B secondary;
    public int calls;
    public Option option;
    public RuntimeException failure;
}
