package fixture;
public final class Provider {
 public static <V extends Bound> V value(Spec<V> spec,long time,V first,V second,V third){
  Trace.hit(1);
  V result=spec.value(time*1000000L,first,second,third);
  Trace.result=result;Trace.hit(2);return result;
 }
}
