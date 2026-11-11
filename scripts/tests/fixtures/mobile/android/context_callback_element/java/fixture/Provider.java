package fixture;
public final class Provider {
 static { Trace.hit(1); }
 private static final <T> T make(Frame frame,Fn<? super Context,? extends T> callback) {
  Trace.frame=frame;Trace.flags=frame.value;
  Context context=(Context)Trace.input;Trace.context=context;Trace.hit(2);
  Object saved=callback.apply(context);Trace.saved=saved;Trace.hit(4);
  return (T)Trace.reply;
 }
 public static Object call(Frame frame,Fn callback){return make(frame,callback);}
}
