package fixture;
public class Provider {
    Object read(Object input) {
        Trace.seen = input;
        Trace.reads++;
        Trace.hit(2);
        if (input != null) ((int[]) input)[0]++;
        return Trace.result;
    }
    public Payload self(Object input) {
        return new Payload(read(input), this);
    }
}
