import fixture.*;
public final class ClosedDefaultAccessCaller {
    public static Vector<Bound> call(DefaultApi receiver,Link<String,Bound> link) {
        return DefaultApi.DefaultImpls.work(receiver,link);
    }
}
