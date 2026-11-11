package fixture;
public final class Core {
    public final Object selected;
    public final int number;
    public Core(Value value, int number) {
        Trace.observe(this, 1);
        if (Trace.first() && value.number != 0 && Trace.second()) {
            this.selected = Trace.choose(value);
            Trace.observe(this, 4);
        } else {
            this.selected = value;
            Trace.observe(this, 5);
        }
        this.number = number;
        Trace.observe(this, 6);
    }
}
