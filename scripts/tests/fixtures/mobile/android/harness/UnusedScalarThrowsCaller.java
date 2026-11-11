import fixture.*;
import java.io.IOException;
public final class UnusedScalarThrowsCaller {
    public static <Z> int generic(Z value)throws IOException {return Core.call(value);}
    public static int checked(Object value)throws IOException {Context context=(Context)value;return Core.receiver().read(context);}
    public static int typed(Context value)throws IOException {return Core.receiver().read(value);}
}
