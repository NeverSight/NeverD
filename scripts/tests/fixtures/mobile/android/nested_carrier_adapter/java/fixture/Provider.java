package fixture;
public final class Provider {
 static {Trace.hit(1);}
 @SuppressWarnings("unchecked")
 public static <T> Pair<Box<T>,Box<Object>> adapt(Pair<T,?> input){
  Trace.seen=input;Trace.hit(2);
  Trace.left=input.left;Trace.right=input.right;Trace.hit(3);
  Object result=Trace.result;Trace.hit(4);
  return (Pair<Box<T>,Box<Object>>)result;
 }
}
