import fixture.*;
import java.io.IOException;
public final class ArrayStarNestedMemberCaller {
    public static Object call(int operation, Impl factory, Object value, String context, Class<?>[] types) throws IOException {
        if (operation == 0) return Core.direct(factory, value, types);
        if (operation == 1) return Core.concrete(factory, value, types);
        if (operation == 2) return Core.wrapped(factory, value, types);
        if (operation == 3) return Core.named(factory, value, context, types);
        if (operation == 4) return Core.routed(factory, value, types);
        return Core.routedNamed(factory, value, context, types);
    }
}
