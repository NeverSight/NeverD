package fixture;
public final class Client {
 public static String call(Fn callback){Object result=Provider.call(callback);String text=(String)result;Trace.after=text;Trace.hit(5);return text;}
}
