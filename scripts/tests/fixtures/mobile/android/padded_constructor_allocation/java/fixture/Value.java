package fixture;
public final class Value {
 public final Object a,b,c,d,e,f;
 public Value(Object a,Object b,Object c,Object d,Object e,Object f) {
  Trace.hit(0,this,new Object[]{a,b,c,d,e,f});
  this.a=a;this.b=b;this.c=c;this.d=d;this.e=e;this.f=f;
  Trace.hit(1,this,new Object[]{a,b,c,d,e,f});
 }
}
