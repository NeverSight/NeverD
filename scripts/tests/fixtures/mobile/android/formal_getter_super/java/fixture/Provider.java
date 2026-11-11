package fixture;
public final class Provider<E> {
 public E value;
 public Provider(E value) { this.value=value; }
 public final E fetch() {
  Trace.hit(1);E saved=value;Trace.seenProvider=this;
  value=(E)Trace.replacement;Trace.changedKey=Trace.replacement;Trace.hit(2);
  return saved;
 }
}
