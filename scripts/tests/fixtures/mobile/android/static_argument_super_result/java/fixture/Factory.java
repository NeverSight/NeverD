package fixture;
public final class Factory {
 static {Trace.hit(9);Trace.returned=Trace.prepared;}
 public static Carrier read(Class token) {
  Trace.token=token;Trace.hit(2);return Trace.returned;
 }
}
