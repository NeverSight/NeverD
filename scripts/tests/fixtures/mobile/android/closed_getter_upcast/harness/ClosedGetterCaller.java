import fixture.*;
public final class ClosedGetterCaller {
 public static Child<Object> make(Provider<Object> receiver) {return new Child<Object>(receiver);}
}
