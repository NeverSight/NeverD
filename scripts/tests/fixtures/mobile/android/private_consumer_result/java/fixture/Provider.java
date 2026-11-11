package fixture;
public class Provider {
 public Object token;
 public Provider(Object token){this.token=token;}
 private static final class Result {public final Object token;public Result(Object token){this.token=token;}}
 public static Object result(Object token){return new Result(token);}
 private final <T> Result consume(Fn<? super T,Unit> callback){
  Trace.receiver=this;Trace.token=this.token;Trace.hit(1);
  Unit saved=callback.apply((T)Trace.input);Trace.saved=saved;Trace.hit(3);
  Result result=(Result)Trace.reply;Trace.bound=result;Trace.hit(4);return result;
 }
 public Object call(Provider peer,Fn callback){Object result=peer.consume(callback);Trace.after=result;Trace.hit(5);return result;}
}
