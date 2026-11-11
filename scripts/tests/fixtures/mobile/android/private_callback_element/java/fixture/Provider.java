package fixture;
public final class Provider {
 static { Trace.hit(1); }
 private static final <T> T make(Fn<? super Context,? extends T> callback) {
  Context context=(Context)Trace.input;Trace.context=context;Trace.hit(2);
  Object saved=callback.apply(context);Trace.saved=saved;Trace.hit(4);
  return (T)Trace.reply;
 }
 public static Object call(Fn callback){return make(callback);}
}
