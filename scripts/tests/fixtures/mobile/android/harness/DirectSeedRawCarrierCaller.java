import fixture.Core;
import fixture.Node;
import fixture.Context;
public final class DirectSeedRawCarrierCaller {
    public static Node call(Object value,Context previous){return Core.call(value,previous);}
    public static Node checked(Object value,Object previous){return Core.checked(value,previous);}
}
