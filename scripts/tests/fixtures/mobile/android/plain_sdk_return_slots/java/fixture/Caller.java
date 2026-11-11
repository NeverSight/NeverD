package fixture;
public final class Caller {
    private Caller() {}
    public static State direct(Reader receiver) { return receiver.sourceState(); }
    public static State inherited(ForwardReader receiver) { return receiver.sourceState(); }
    public static State contract(Api receiver) { return receiver.sourceState(); }
    public static int scalar(SdkRoot receiver) { return receiver.getState(); }
}
