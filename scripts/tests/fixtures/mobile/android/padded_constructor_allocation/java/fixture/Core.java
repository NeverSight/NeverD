package fixture;
public final class Core {
 public final Value value;
 public Core(Value value) {
  this.value=value;
  Trace.hit(2,this,new Object[]{value});
 }
 public Core(Object a,Object b,Object c,Object d,Object e,Object f) {
  this(new Value(a,b,c,d,e,f));
 }
}
