package fixture;
public final class Client {
 public static Object call(Provider caller,Provider peer,Fn callback){Object result=caller.call(peer,callback);Trace.last=result;Trace.hit(6);return result;}
}
