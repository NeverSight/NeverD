package fixture;
public final class Subject extends Base {
    public Subject() {}
    @Override @SuppressWarnings("unchecked")
    protected <T extends Marker> T read() {
        Trace.hit(4, this); Marker result = Trace.value; saved = result;
        Trace.hit(5, this); return (T)result;
    }
}
