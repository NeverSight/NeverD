import fixture.*;
import java.io.IOException;
public final class ConditionalArrayLengthCaller {
    public static Target call(Holder holder, boolean flag) throws IOException {
        return Core.make(holder, flag);
    }
}
