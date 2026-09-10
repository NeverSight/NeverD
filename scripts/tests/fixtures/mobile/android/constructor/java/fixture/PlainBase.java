package fixture;

public class PlainBase {
    public static int objectCalls;
    public static int sequenceCalls;
    public int tag;
    public Object received;

    public PlainBase(Object value) {
        objectCalls++;
        tag = 101;
        received = value;
    }

    public PlainBase(CharSequence value) {
        sequenceCalls++;
        tag = 202;
        received = value;
    }
}
