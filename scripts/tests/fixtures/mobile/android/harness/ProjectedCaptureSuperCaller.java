import fixture.*;
public final class ProjectedCaptureSuperCaller {
    public static Base make(Owner<UpperBound> owner,UpperBound token,long wide,Sink<Object> completion) {
        return owner.make(token,wide,completion);
    }
}
