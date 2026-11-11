package fixture;
public final class Core<T> implements Marker<T> {
 public final Box<? extends T> values;
 public Core(){this((Box<? extends T>)(Box)Provider.make());Trace.hit(6);}
 public Core(Box<? extends T> values){Trace.partial=this;Trace.hit(3);this.values=values;Trace.hit(4);Trace.element=values.value;Trace.hit(5);}
 public Core(Marker<T> ignored){this.values=null;Trace.hit(9);}
}
