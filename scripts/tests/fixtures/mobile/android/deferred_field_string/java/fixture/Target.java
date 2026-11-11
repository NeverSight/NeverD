package fixture;
public final class Target {
    public String text;
    public Object marker, value;
    public boolean flag;
    static { Trace.hit(1, null); }
    public Target(String text, Object marker, boolean flag, Object value) {
        this.text = text; this.marker = marker; this.flag = flag; this.value = value;
        Trace.hit(2, this);
    }
}
