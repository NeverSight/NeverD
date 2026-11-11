import fixture.*;
public final class PrivateContextThrowsCaller {
  public static Object invoke(Core receiver, int operation, Context context, Carrier<Object> callback) throws java.io.IOException {
    return operation == 0 ? receiver.call(context, callback) : Core.callStatic(context, callback);
  }
}
