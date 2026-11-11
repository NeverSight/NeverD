package fixture;
public final class Core<E> {
 static { Trace.hit(9); }
 public final E value;
 public final Object previous,next;
 public Core(E value) { this(value,Provider.first,Provider.second); }
 public Core(E value,Object previous) { this(value,previous,Provider.second); }
 public Core(E value,Object previous,Object next) {
  Trace.hit(2);this.value=value;this.previous=previous;this.next=next;
  Trace.seen=this;Provider.first=Trace.replacement;Provider.second=Trace.replacement;
  Trace.hit(3);
 }
}
