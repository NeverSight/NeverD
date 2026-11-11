package fixture;
public class Slice<A,B> implements View<A> {
    public A first;
    public B second;
    public Slice(A first,B second) {this.first=first;this.second=second;}
    public A head() throws Failure {Trace.hit(3);return first;}
}
