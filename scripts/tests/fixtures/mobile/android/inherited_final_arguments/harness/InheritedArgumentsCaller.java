import fixture.*;
public final class InheritedArgumentsCaller {
 public static Child<Object,Object> make(Object item,String tag) {
  return new Child<Object,Object>(item,tag);
 }
}
