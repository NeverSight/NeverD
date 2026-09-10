package fixture;

public final class ThisChoice {
    public static int objectCalls;
    public static int sequenceCalls;
    public static int delegatingCalls;
    public int tag;
    public int marker;
    public Object received;

    public ThisChoice(Object value) {
        objectCalls++;
        tag = 303;
        received = value;
    }

    public ThisChoice(CharSequence value) {
        sequenceCalls++;
        tag = 404;
        received = value;
    }

    public <T extends Object & CharSequence> ThisChoice(T value, int marker) {
        this((Object) value);
        delegatingCalls++;
        this.marker = marker;
    }
}
