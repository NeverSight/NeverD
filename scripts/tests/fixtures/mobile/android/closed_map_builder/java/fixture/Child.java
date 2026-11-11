package fixture;
public final class Child extends Parent<Key<Object>,Box<Object>> {
 static {Trace.hit(4);}
 public final Snapshot snapshot;
 @SuppressWarnings("unchecked")
 public Child(Snapshot snapshot){
  super((java.util.HashMap<Key<Object>,Box<Object>>)(Object)snapshot);
  Trace.hit(7);this.snapshot=snapshot;Trace.hit(8);
 }
}
