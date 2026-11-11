package fixture;
public final class Child<K,V> extends Middle<K,V,K> {
 public final Object item;
 public final String tag;
 public Target output;
 public Child(Object item,String tag) {
  this.item=item;this.tag=tag;Trace.owner=this;
  output=new Target(select(this.item,this.tag),select(Trace.alternate,Trace.nextTag));
  Trace.hit(4);
 }
}
