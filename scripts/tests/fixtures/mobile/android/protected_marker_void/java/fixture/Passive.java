package fixture;

public class Passive extends Base {
    public <U extends Marker> void forward(U input, Base context, int tag) {
        mark(tag);
        consume(input, context);
        Trace.hit(4, this, input, context);
    }
}
