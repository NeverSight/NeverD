package fixture;
public class Parent<K,V> {
 static {Trace.hit(3);}
 public final java.util.HashMap<K,V> inherited;
 @SuppressWarnings({"unchecked","rawtypes"})
 public Parent(java.util.HashMap<K,V> map){
  Trace.seen=map;Trace.partial=this;Trace.hit(5);
  this.inherited=map;
  if(Trace.mutate)((java.util.HashMap)map).put(Trace.key,Trace.value);
  Trace.hit(6);
 }
}
