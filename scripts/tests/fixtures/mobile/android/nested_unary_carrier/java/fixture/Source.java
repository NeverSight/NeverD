package fixture;
public final class Source<E> {
 public E value;
 public Source(E value){this.value=value;}
 public E read(){E chosen=value;Trace.hit(1,this,chosen,null);return chosen;}
}
