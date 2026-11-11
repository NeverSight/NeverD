import fixture.*;
public final class ErasedSuperConstructorCaller {
    public static Child make(Sink<? super Child> value) { return new Child(value); }
}
