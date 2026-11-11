package fixture;
public final class Alternate extends Base {
    public Alternate() {}
    @Override @SuppressWarnings("unchecked")
    public <T extends Marker> T read() {
        Trace.hit(4, this); Marker result = Trace.value; saved = result;
        Trace.hit(5, this); return (T)result;
    }
}
