package fixture;
import java.util.List;
public final class Provider {
 static { Trace.hit(1); }
 private static final <T,A extends Target> A collect(List<? extends T> values,A target,CharSequence head,int limit,Fn<? super T,? extends CharSequence> callback) {
  Trace.values=values;Trace.target=target;Trace.head=head;Trace.limit=limit;Trace.callback=callback;Trace.hit(2);
  T element=values.get(0);Trace.element=element;Trace.hit(3);
  CharSequence text=callback.apply(element);Trace.text=text;Trace.hit(5);
  Trace.hit(6);target.append(head);target.append(text);
  return (A)Trace.result;
 }
 public static Target call(List values,Target target,CharSequence head,int limit,Fn callback) {
  return collect(values,target,head,limit,callback);
 }
}
