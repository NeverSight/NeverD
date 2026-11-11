import fixture.*;
import java.io.IOException;
public final class NominalSeedContextThrowsCaller {
    public static <Z> Context generic(Z value,boolean left,boolean right)throws IOException {return Core.call(value,left,right);}
    public static Context checked(Object value,boolean left,boolean right)throws IOException {return Core.checked(value,left,right);}
    public static Context typed(Context value,boolean left,boolean right)throws IOException {return Core.call(value,left,right);}
}
