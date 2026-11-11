package fixture;
public final class Client {
 public static Context call(Fn callback){Result result=Provider.call(callback);Trace.boundResult=result;Context text=(Context)result;Trace.after=text;Trace.hit(5);return text;}
}
