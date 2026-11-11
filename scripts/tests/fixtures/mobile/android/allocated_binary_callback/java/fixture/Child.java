package fixture;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Child extends Parent {
 public final long copy;
 public Child(long id,Marker marker){super(id,marker,null,new Adapter());Trace.child=this;Trace.hit(7);copy=id;}
}
