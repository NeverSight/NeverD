package fixture;
public final class Core {
 static { Trace.hit(9); }
 public static Object[] make(Object[] source,int index,Object key,Object value) {
  return splice(source,index,key,value);
 }
 private static final <K,V> Object[] splice(Object[] source,int index,K key,V value) {
  Trace.hit(1);Trace.hit(2);source[index]=key;
  Trace.hit(3);source[index+1]=value;Trace.hit(4);
  Object[] result=Trace.returnInput?source:new Object[source.length+2];
  if(result!=source) System.arraycopy(source,0,result,1,source.length);
  Trace.result=result;Trace.hit(5);return result;
 }
}
