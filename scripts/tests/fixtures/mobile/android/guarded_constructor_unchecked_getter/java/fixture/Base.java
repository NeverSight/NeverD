package fixture;

public class Base {
    public final Object value;
    public Base(Object value) {
        Trace.bases++;
        Trace.baseValue = value;
        this.value = value;
        Trace.stage = 4;
    }
}
