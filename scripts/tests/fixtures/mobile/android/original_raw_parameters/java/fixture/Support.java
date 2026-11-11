package fixture;
public final class Support {
    private Support() {}
    @SuppressWarnings("rawtypes")
    public static void forward(Sink sink, RawBox box, Object tag) {
        Trace.events = Trace.events * 10 + 3;
        sink.accept(box, tag);
    }
}
