import fixture.Core;
public final class ScopedConstructorCaller {
 public static Core<Object> make(Object value,Object previous,boolean pair) {
  return pair?new Core<Object>(value,previous):new Core<Object>(value);
 }
}
