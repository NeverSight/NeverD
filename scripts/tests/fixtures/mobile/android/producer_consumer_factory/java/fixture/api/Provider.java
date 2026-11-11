package fixture.api;
import java.util.*;
import fixture.Trace;
class Provider {
 public static <T> List<T> build(Iterable<? extends T> input,Comparator<? super T> action) {
  Trace.hit(0,input,action);
  Iterator<? extends T> iterator=input.iterator();
  T first=iterator.next();Trace.hit(1,first,action);
  T second=iterator.next();Trace.hit(2,second,action);
  int decision=action.compare(first,second);
  Trace.hit(3,first,second);
  ArrayList<T> result=new ArrayList<T>();
  if(decision<=0){result.add(first);result.add(second);}
  else{result.add(second);result.add(first);}
  Trace.hit(4,result,action);
  return result;
 }
}
