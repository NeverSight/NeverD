package fixture;
public class Base {
    public static Base latest;
    public Carrier<? extends Base> source;
    public Base value;
    public Object marker;
    public int events;
    public Base() { events = 7; }
    public Base(String unused) { events = 8; }
    public Base(Carrier<? extends Base> source) {
        latest = this;
        this.source = source;
        events = 1;
        marker = source.probe.observe(this, source);
        events = events * 10 + 2;
        value = source.value;
        if (source.failure != null) throw source.failure;
        events = events * 10 + 3;
    }
}
