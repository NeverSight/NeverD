import fixture.*;
public final class ConcreteSuperCaller {
 public static boolean call(Child<?> receiver,Object value) {return receiver.touch(value);}
}
