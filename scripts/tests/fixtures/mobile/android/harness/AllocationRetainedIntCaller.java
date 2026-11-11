import fixture.*;
public final class AllocationRetainedIntCaller {
    public static Outer call(Producer producer) { return Core.make(producer); }
}
