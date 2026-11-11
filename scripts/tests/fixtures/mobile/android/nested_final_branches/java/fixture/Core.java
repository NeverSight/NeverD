package fixture;
public class Core {
    public final Delegate selected;
    public final int first, second;
    public final long stamp;
    public Object extra;
    public Core(Object activity, Object toolbar, Object supplied, int first, int second, long stamp) {
        Trace.observe(this, 1);
        if (toolbar != null) {
            this.selected = new Primary(toolbar);
            Trace.observe(this, 2);
        } else if (activity instanceof Provider) {
            this.selected = ((Provider) activity).get();
            Trace.observe(this, 3);
        } else {
            this.selected = new Alternate(activity);
            Trace.observe(this, 4);
        }
        this.first = first; this.second = second; this.stamp = stamp;
        Trace.observe(this, 5);
        if (supplied == null) this.extra = new Token(this.selected.read());
        else this.extra = supplied;
        Trace.observe(this, 6);
    }
}
