import fixture.*;
public final class InstanceSeedNominalCaller {
    public static <Z> Provider generic(Z value,boolean left,boolean right){return Core.call(value,left,right);}
    public static Provider checked(Object value,boolean left,boolean right){return Core.checked(value,left,right);}
    public static Provider typed(String value,boolean left,boolean right){return Core.call(value,left,right);}
}
