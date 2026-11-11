package fixture;

public abstract class Base {
    public Marker saved;
    public Base context;
    public Object seen;
    public int marker;
    public int writes;

    protected void mark(int value) {
        marker = value;
        Trace.hit(0, this, null, null);
    }

    protected <T extends Marker> void consume(T input, Base context) {
        Trace.hit(1, this, input, context);
        saved = input;
        this.context = context;
        Trace.hit(2, this, input, context);
        context.seen = input;
        writes++;
        Trace.hit(3, this, input, context);
    }

    public void write(Marker input, Base context, int tag) {
        mark(tag);
        consume(input, context);
        Trace.hit(4, this, input, context);
    }
}
