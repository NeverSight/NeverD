package fixture;

public final class Subject extends Passive {
    @Override protected <T extends Marker> void consume(T input, Base context) {
        Trace.hit(5, this, input, context);
        saved = input;
        this.context = context;
        Trace.hit(6, this, input, context);
        context.seen = input;
        writes++;
        Trace.hit(7, this, input, context);
    }
}
