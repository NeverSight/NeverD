package fixture;
public final class Target {
 static { Trace.hit(9);Trace.current=Trace.initialized; }
 public final Object left,right;
 public Target(Object left,Object right) {
  Trace.partial=this;this.left=left;Trace.hit(3);this.right=right;Trace.hit(4);
 }
}
