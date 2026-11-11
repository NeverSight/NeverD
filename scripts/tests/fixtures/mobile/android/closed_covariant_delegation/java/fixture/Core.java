package fixture;
public class Core {
    public static Core latest;
    public String key;
    public Object marker;
    public int events, finished;
    public Core(Carrier<? extends Bound> source) {
        this(Names.key(source.getClass()));
        finished = 9;
    }
    public Core(String value) {
        latest = this;
        key = value;
        events = 1;
        marker = Names.bodyProbe.observe(this, value);
        events = events * 5 + 2;
        if (Names.bodyFailure != null) throw Names.bodyFailure;
        events = events * 5 + 3;
        finished = 7;
    }
}
