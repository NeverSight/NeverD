package fixture;

public class Provider {
    public long number;
    public double wide;
    public float narrow;
    public int integer;
    public boolean flag;

    public boolean readBoolean(Object input) {
        Trace.hit(1);
        Trace.consume(input);
        number = 0; wide = 0; narrow = 0; integer = 0;
        return flag;
    }
}
