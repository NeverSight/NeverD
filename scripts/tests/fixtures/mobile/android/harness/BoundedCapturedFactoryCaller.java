import fixture.*;
public class BoundedCapturedFactoryCaller {
    public static Outer.Member call(Outer outer, Converter converter, String label, int mode) {
        return Caller.call(outer,converter,label,mode);
    }
}
