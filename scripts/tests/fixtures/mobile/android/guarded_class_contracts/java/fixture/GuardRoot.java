package fixture;
public class GuardRoot<R,S> implements CountView<R> {
    public int rootCalls;
    public GuardRoot() {}
    public boolean contains(Object value) {
        rootCalls++;
        return true;
    }
}
