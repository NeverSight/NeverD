package fixture;
public final class Subject extends Base {
    public Subject() {}
    @Override @SuppressWarnings("unchecked")
    public <T extends Marker> T read() {
        Trace.hit(1, this); Marker result = Trace.value; saved = result;
        Trace.hit(2, this); return (T)result;
    }
}
