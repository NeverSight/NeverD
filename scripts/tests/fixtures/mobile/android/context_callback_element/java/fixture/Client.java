package fixture;
public final class Client {
 public static String call(Frame frame,Fn callback){Object result=Provider.call(frame,callback);String text=(String)result;Trace.after=text;Trace.hit(5);return text;}
}
