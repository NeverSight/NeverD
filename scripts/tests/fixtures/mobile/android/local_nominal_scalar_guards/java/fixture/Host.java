package fixture;
public final class Host {
    private Host() { }
    public static Api<String> create(final Trace trace) {
        return new Api<String>() {
            public boolean contains(byte input) {
                ++trace.calls;
                trace.marker = trace.marker * 5 + 1;
                trace.lastByte = input;
                if (trace.failure != null) throw trace.failure;
                return input == trace.expectedByte;
            }
            public int index(long input) {
                ++trace.calls;
                trace.marker = trace.marker * 5 + 2;
                trace.lastLong = input;
                if (trace.failure != null) throw trace.failure;
                return input == trace.expectedLong ? 13 : -3;
            }
            public final boolean contains(Object input) {
                if (!(input instanceof ByteBox)) return false;
                return contains(((Carrier) input).readByte());
            }
            public final int index(Object input) {
                if (!(input instanceof LongBox)) return -1;
                return index(((Carrier) input).readLong());
            }
            public <R, S> R echo(R input, S tag) { return input; }
        };
    }
}
