import fixture.*;
import java.io.IOException;
public final class ClosedHeadMemberCaller {
    public static Object call(int operation, Impl factory, Class type) throws IOException {
        if (operation == 0) return Core.direct(factory, type);
        if (operation == 1) return Core.concrete(factory, type);
        if (operation == 2) return Core.pick(factory, type);
        return Core.wrapped(factory, type);
    }
}
