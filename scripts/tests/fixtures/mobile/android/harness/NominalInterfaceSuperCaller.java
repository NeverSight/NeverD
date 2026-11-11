package probe;
import fixture.*;
public final class NominalInterfaceSuperCaller {
    public static Output call(Owner owner, Input value) {
        Trace.hit(6);
        Output result = owner.convert(value);
        Trace.hit(0);
        return result;
    }
    public static Output direct(Owner owner, Input value) { return owner.convert(value); }
}
