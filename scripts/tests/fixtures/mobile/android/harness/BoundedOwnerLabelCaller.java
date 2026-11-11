import fixture.*;
public final class BoundedOwnerLabelCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Core make(Object seed,Link converter,Object threshold) {
        return new Core<Object,Bound>(seed,converter,threshold);
    }
}
