package fixture;
public class Path<K,V,T> {
 static { Trace.hit(9); }
 public int serial;
 public Path() { this.serial=++Trace.started;Trace.partialElement=this;Trace.hit(1); }
}
