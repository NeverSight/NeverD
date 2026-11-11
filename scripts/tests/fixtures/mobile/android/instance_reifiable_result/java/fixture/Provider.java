package fixture;

public class Provider extends Carrier<Provider> {
    public final Object marker;
    public Provider(Object marker) { this.marker = marker; }
    @SuppressWarnings("unchecked")
    public <T> T read(String name) throws Failure {
        Trace.calls++;
        Trace.stage = 1;
        Trace.receiver = this;
        Trace.name = name;
        Trace.observedMarker = marker;
        Trace.length = name.length();
        Trace.stage = 2;
        Trace.action.run();
        Trace.stage = 3;
        return (T)Trace.value;
    }
    public <T> T quiet(String name) {
        try { return this.<T>read(name); }
        catch (Exception ignored) { Trace.caught++; Trace.caughtFailure = ignored; return null; }
    }
    public <T> T empty() throws Failure { return this.<T>read(Trace.key); }
}
