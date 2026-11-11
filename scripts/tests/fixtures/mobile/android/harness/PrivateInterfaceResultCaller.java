import fixture.*;
import java.util.List;
public final class PrivateInterfaceResultCaller {
 public static Target call(List values,Target target,CharSequence head,int limit,Fn callback) { return Client.call(values,target,head,limit,callback); }
}
