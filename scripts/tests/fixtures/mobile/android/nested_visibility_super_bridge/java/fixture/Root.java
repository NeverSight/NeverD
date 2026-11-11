package fixture;
class Root<V extends Bound> {
    public int index() {
        Trace.hit(1); Trace.receiver = this; Trace.hit(2);
        Trace.action.run(); Trace.hit(3); return Trace.integer;
    }
    public boolean flag() {
        Trace.hit(1); Trace.receiver = this; Trace.hit(2);
        Trace.action.run(); Trace.hit(3); return Trace.flag;
    }
    public long wide() {
        Trace.hit(1); Trace.receiver = this; Trace.hit(2);
        Trace.action.run(); Trace.hit(3); return Trace.wide;
    }
    public double ratio() {
        Trace.hit(1); Trace.receiver = this; Trace.hit(2);
        Trace.action.run(); Trace.hit(3); return Trace.ratio;
    }
    public Object value() {
        Trace.hit(1); Trace.receiver = this; Trace.hit(2);
        Trace.action.run(); Trace.hit(3); return Trace.value;
    }
    public void touch(Context context, V input, long bits) {
        Trace.hit(1); Trace.receiver = this; Trace.context = context;
        Trace.input = input; Trace.bits = bits; Trace.hit(2);
        Trace.action.run(); Trace.hit(3);
    }
}
