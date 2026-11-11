package fixture;
public class Provider<A,B> implements Catalog<A> {
    public Slice<A,B> result;
    public Provider() {}
    public Slice<A,B> view() throws Failure {
        Trace.hit(1);
        Slice<A,B> saved=result;
        Trace.hit(2);
        return saved;
    }
}
