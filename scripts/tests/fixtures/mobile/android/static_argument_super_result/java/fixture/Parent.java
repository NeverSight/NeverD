package fixture;
public class Parent<T> {
 public final Carrier<T> carrier;
 public Parent(Carrier<T> carrier) {
  Trace.parent=this;Trace.hit(3);this.carrier=carrier;Trace.hit(4);
 }
}
