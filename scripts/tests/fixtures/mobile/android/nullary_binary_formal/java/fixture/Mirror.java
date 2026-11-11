package fixture;
public interface Mirror<A,B> {
    B apply(A value);
    @SuppressWarnings("unchecked")
    public static <A> Mirror<A,A> build() {
        Trace.calls++;
        Trace.stage=1;
        Trace.action.run();
        Trace.stage=2;
        return (Mirror<A,A>)Trace.result;
    }
}
