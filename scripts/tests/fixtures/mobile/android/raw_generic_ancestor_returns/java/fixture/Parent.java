package fixture;
import java.util.Iterator;
@SuppressWarnings("rawtypes")
public class Parent implements Root {
    public Iterator value;
    public int[] marker, observed;
    public int calls;
    public RuntimeException failure;
    public Iterator values() {
        calls++;
        observed = marker;
        marker[0] += 3;
        if (failure != null) throw failure;
        return value;
    }
}
