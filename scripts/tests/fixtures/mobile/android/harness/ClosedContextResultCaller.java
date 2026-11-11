import fixture.*;
import java.util.Map;
public final class ClosedContextResultCaller {
 public static Object call(int operation, Factory provider, Map<String,Object> values) throws Throwable {
  if (operation == 0) return Core.direct(provider, values);
  if (operation == 1) return Core.wrapped(provider, values);
  if (operation == 2) { String result = Core.<String>contextual(provider, values); return result; }
  if (operation == 3) return Core.staticCall(provider, values);
  return Core.token(provider, values);
 }
}
