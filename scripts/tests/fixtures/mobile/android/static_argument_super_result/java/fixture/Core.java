package fixture;
public final class Core<A,B> extends Parent<Payload<?,?>> {
 public final Carrier<A> first;
 public final Carrier<B> second;
 public Core(Carrier<A> first,Carrier<B> second) {
  Trace.hit(1);
  super(Factory.read(Payload.class));
  this.first=first;Trace.hit(5);this.second=second;Trace.hit(6);
 }
}
