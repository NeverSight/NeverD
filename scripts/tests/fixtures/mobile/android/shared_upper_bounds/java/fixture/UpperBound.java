package fixture;

public class UpperBound {
    public Object value;
    public int reads;
    public RuntimeException failure;
    public Object inspect() {
        ++reads;
        if (failure != null) throw failure;
        return value;
    }
}
