import fixture.*;
public final class OverloadedInterfaceSuperCaller {
    public static float callInt(Root receiver,int value,int view) { return Facade.callInt(receiver,value,view); }
    public static double callLong(Root receiver,long value,int view) { return Facade.callLong(receiver,value,view); }
}
