import fixture.*;
import java.util.Map;
public final class FormalGetterCaller {
 public static Child<Object,Object> make(Map<Object,Provider<Object>> map,Object key,Provider<Object> receiver) {
  return new Child<Object,Object>(map,key,receiver);
 }
}
