package fixture;
import java.util.List;
public final class Provider {
 static { Trace.hit(1); }
 public static final <T> String collect(List<? extends T> values,Target target,CharSequence head,int limit,Fn<? super T,? extends CharSequence> callback) {
  Trace.values=values;Trace.target=target;Trace.head=head;Trace.limit=limit;Trace.callback=callback;Trace.hit(2);
  T element=values.get(0);Trace.element=element;Trace.hit(3);
  CharSequence text=callback.apply(element);Trace.text=text;Trace.hit(5);
  Trace.hit(6);target.append(head);target.append(text);
  return (String)Trace.result;
 }
 public static String call(List values,Target target,CharSequence head,int limit,Fn callback) {
  return collect(values,target,head,limit,callback);
 }
}
