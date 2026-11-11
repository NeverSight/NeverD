package fixture;
public final class Core {
 static { Trace.hit(9); }
 public static Object[] make(Object[] source,int index,Object key) {
  return splice(source,index,key);
 }
 private static final <E> Object[] splice(Object[] source,int index,E key) {
  Trace.hit(1);Trace.hit(2);source[index]=key;
  Trace.hit(3);Trace.hit(4);
  Object[] result=Trace.returnInput?source:new Object[source.length+1];
  if(result!=source) System.arraycopy(source,0,result,1,source.length);
  Trace.result=result;Trace.hit(5);return result;
 }
}
