import fixture.*;
public class CapturedConstructorReadCaller {
    public static Base make(Owner owner, Object payload, boolean wide) {
        if (wide) return owner.new WideWorker(payload);
        return owner.new Worker(payload);
    }
}
