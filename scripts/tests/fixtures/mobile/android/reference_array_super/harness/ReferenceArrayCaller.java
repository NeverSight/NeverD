import fixture.Builder;
import fixture.Child;
public final class ReferenceArrayCaller {
 public static Child<Object,Object> make(Builder<Object,Object> builder,int count) {
  return new Child<Object,Object>(builder,count);
 }
}
