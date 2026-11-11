package fixture;

public class Cell<A, B> {
    public A first;
    public B second;
    public int reads;
    public B read() {
        ++reads;
        return second;
    }
}
