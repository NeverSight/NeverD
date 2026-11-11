import fixture.*;
public final class MethodSeedBridgeCaller {
    public static <Z> Closed generic(Z value,boolean left,boolean right){return ((Closed)Core.receiver()).accept(value,left,right);}
    public static Middle checked(Object value,boolean left,boolean right){String input=(String)value;return ((Middle)Core.receiver()).accept(input,left,right);}
    public static Provider typed(String value,boolean left,boolean right){return Core.receiver().accept(value,left,right);}
}
