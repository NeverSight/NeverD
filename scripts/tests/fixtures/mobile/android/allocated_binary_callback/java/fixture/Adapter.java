package fixture;
@SuppressWarnings("rawtypes")
public final class Adapter implements Callback {
 static {Trace.hit(2);}
 public Adapter(){Trace.adapter=this;Trace.hit(3);}
 public Object invoke(Object value){Trace.seen=value;Trace.hit(9);return Trace.result;}
}
