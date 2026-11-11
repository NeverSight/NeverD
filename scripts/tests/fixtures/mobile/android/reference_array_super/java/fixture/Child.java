package fixture;
public final class Child<K,V> extends Parent<K,V,K> {
 public final Object marker;
 public final int done;
 @SuppressWarnings({"rawtypes","unchecked"})
 public Child(Builder<K,V> builder, int count) {
  Path<K,V,K>[] path=new Path[count];
  for(int i=0;i<count;++i) path[i]=new Element<K,V>();
  super(builder,path);
  this.marker=builder.marker;Trace.hit(5);
  this.done=count;Trace.hit(6);
 }
}
