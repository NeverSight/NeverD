package fixture;
import java.io.IOException;
public final class InterfaceTokenResultThrowsCaller {
    public static Object call(int operation,Core core,Class<?> token) throws IOException {
        if(operation==0) return Core.call(core,token);
        return Core.checked(core,token);
    }
}
