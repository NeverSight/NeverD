package fixture;
public class Base {
    public Marker saved;
    public Base() {}
    @SuppressWarnings("unchecked")
    protected <T extends Marker> T relay(String key, Base context) {
        if (key == null || context != this) throw new IllegalStateException();
        return read();
    }
    @SuppressWarnings("unchecked")
    protected <T extends Marker> T read() {
        Trace.hit(1, this); Marker result = Trace.value; saved = result;
        Trace.hit(2, this); return (T)result;
    }
}
