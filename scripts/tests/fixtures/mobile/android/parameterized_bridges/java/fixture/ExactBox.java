package fixture;

public class ExactBox implements Nested<Value> {
    public Box<? super Value> copy(Box<? super Value> value) { return value; }
}
