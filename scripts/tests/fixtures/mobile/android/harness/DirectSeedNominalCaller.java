import fixture.*;
public final class DirectSeedNominalCaller {
    public static <Z> Provider generic(Z value){return Core.call(value);}
    public static Provider checked(Object value){return Core.checked(value);}
    public static Provider typed(String value){return Core.call(value);}
}
