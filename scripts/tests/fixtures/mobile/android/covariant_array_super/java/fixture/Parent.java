package fixture;
public class Parent<K,V,T> {
 static { Trace.hit(7); }
 public final Builder<K,V> builder;
 public final Path<K,V,T>[] path;
 public Parent(Builder<K,V> builder, Path<K,V,T>[] path) {
  this.builder=builder;this.path=path;Trace.parent=this;Trace.hit(3);
  if(path.length!=0) { Trace.first=path[0];path[0]=null; }
  Trace.hit(4);
 }
 public Parent(Object builder, Object[] path) {
  this.builder=null;this.path=null;throw new AssertionError("wrong array constructor overload");
 }
}
