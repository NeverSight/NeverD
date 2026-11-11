package fixture;
public final class Output {
    public int bits;
    public Output(int value) {
        bits = value;
        Trace.constructed = this;
        Trace.outputCalls++;
        Trace.hit(4);
        if (Trace.outputAction != null) Trace.outputAction.run();
    }
}
