package fixture;public final class Provider {
 public static <K,V> Result<K,V> make(Pair<? extends K,? extends V>... items){
  Trace.array=items;Trace.hit(1);
  for(Pair<? extends K,? extends V> item:items){Trace.first=item.first;Trace.second=item.second;Trace.hit(2);}
  if(items.length!=0)items[0]=null;
  Trace.hit(3);
  return (Result<K,V>)Trace.output;
 }
}
