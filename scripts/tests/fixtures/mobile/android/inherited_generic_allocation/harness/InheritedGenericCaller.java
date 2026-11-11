import fixture.*;
public final class InheritedGenericCaller {
 public static Target make(Child<?,?> receiver) { return receiver.make(); }
}
