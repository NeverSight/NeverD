package fixture;
import java.util.List;
public final class Client {
 public static String call(List values,Target target,CharSequence head,int limit,Fn callback) {
  String result=Provider.call(values,target,head,limit,callback);Trace.after=result;Trace.hit(9);return result;
 }
}
