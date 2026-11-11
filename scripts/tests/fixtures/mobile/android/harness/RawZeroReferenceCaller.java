import fixture.*;
public final class RawZeroReferenceCaller {
    public static Object call(Producer producer) { return producer.get(); }
}
